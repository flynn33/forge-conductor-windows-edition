#include "WindowsTaskSchedulerStartupPlatform.h"

#include <Windows.h>
#include <oleauto.h>
#include <taskschd.h>

#include "ManagerStartupOwnershipProjector.h"
#include "OperationContextGuard.h"
#include "TaskSchedulerDurationCodec.h"
#include "UniqueBstr.h"
#include "UniqueComInterface.h"
#include "UniqueVariant.h"
#include "UtfConversion.h"
#include "Win32Error.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::size_t MaximumNativeTextUtf16Units =
    Manager::ManagerStartupTaskPolicy::MaximumTextBytes;
constexpr std::size_t MaximumAccountDomainUtf16Units = 256U;
constexpr std::chrono::seconds DefaultTaskAndTriggerExecutionTimeLimit{
    72 * 60 * 60};

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] Domain::Result<void> exactComResult(
    const HRESULT result,
    const std::string_view action,
    const std::string_view code = Domain::ErrorCodes::InternalFailure)
{
    if (result == S_OK) {
        return Domain::Result<void>::success();
    }
    return Domain::Result<void>::failure(
        makeHResultError(action, result, code));
}

[[nodiscard]] Domain::Result<void> contextCheckpoint(
    const Domain::OperationContext& context,
    const std::string_view action)
{
    return validateOperationContext(
        context,
        std::chrono::steady_clock::now(),
        action);
}

[[nodiscard]] Domain::Result<std::string> normalizeCurrentUserIdentity(
    std::string observed,
    const WindowsCurrentUserIdentity& currentIdentity,
    const Domain::OperationContext& context,
    const std::string_view label)
{
    try {
        if (observed == currentIdentity.sidText()) {
            return Domain::Result<std::string>::success(std::move(observed));
        }
        auto checkpoint = contextCheckpoint(
            context, "resolve a Task Scheduler account identity");
        if (!checkpoint) {
            return Domain::Result<std::string>::failure(
                std::move(checkpoint).error());
        }
        auto account = strictUtf8ToUtf16(observed);
        if (!account) {
            return Domain::Result<std::string>::failure(
                std::move(account).error());
        }
        if (account.value().empty()) {
            return failure<std::string>(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{label} + " did not identify the current user.");
        }

        DWORD sidBytes{};
        DWORD domainCharacters{};
        SID_NAME_USE sidUse{};
        ::SetLastError(ERROR_SUCCESS);
        const BOOL sized = ::LookupAccountNameW(
            nullptr,
            account.value().c_str(),
            nullptr,
            &sidBytes,
            nullptr,
            &domainCharacters,
            &sidUse);
        const DWORD sizingError = ::GetLastError();
        if (sized != FALSE || sizingError != ERROR_INSUFFICIENT_BUFFER ||
            sidBytes < 8U ||
            sidBytes > WindowsCurrentUserIdentity::MaximumSidBytes ||
            domainCharacters > MaximumAccountDomainUtf16Units) {
            return failure<std::string>(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{label} +
                    " could not be resolved to a bounded current-user SID.");
        }

        std::vector<std::byte> resolvedSid(
            static_cast<std::size_t>(sidBytes));
        std::vector<wchar_t> resolvedDomain(
            domainCharacters == 0U
                ? 1U
                : static_cast<std::size_t>(domainCharacters),
            L'\0');
        DWORD returnedSidBytes = sidBytes;
        DWORD returnedDomainCharacters = domainCharacters;
        if (::LookupAccountNameW(
                nullptr,
                account.value().c_str(),
                resolvedSid.data(),
                &returnedSidBytes,
                domainCharacters == 0U ? nullptr : resolvedDomain.data(),
                &returnedDomainCharacters,
                &sidUse) == FALSE ||
            returnedSidBytes != sidBytes ||
            returnedDomainCharacters > domainCharacters ||
            ::IsValidSid(resolvedSid.data()) == FALSE ||
            ::GetLengthSid(resolvedSid.data()) != sidBytes) {
            return failure<std::string>(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{label} +
                    " did not resolve to a canonical bounded SID.");
        }

        const auto expectedSid = currentIdentity.sidBytes();
        if (expectedSid.empty() ||
            ::EqualSid(
                resolvedSid.data(),
                const_cast<std::byte*>(expectedSid.data())) == FALSE) {
            return failure<std::string>(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{label} +
                    " resolved to a different Windows account.");
        }
        checkpoint = contextCheckpoint(
            context, "finish resolving a Task Scheduler account identity");
        if (!checkpoint) {
            return Domain::Result<std::string>::failure(
                std::move(checkpoint).error());
        }
        return Domain::Result<std::string>::success(
            currentIdentity.sidText());
    } catch (...) {
        return failure<std::string>(
            Domain::ErrorCodes::InternalFailure,
            std::string{label} + " could not allocate bounded identity state.");
    }
}

[[nodiscard]] Domain::Result<bool> observedBoolean(
    const VARIANT_BOOL value,
    const std::string_view label)
{
    if (value == VARIANT_FALSE) {
        return Domain::Result<bool>::success(false);
    }
    if (value == VARIANT_TRUE) {
        return Domain::Result<bool>::success(true);
    }
    return failure<bool>(
        Domain::ErrorCodes::IntegrityFailure,
        std::string{label} + " returned a noncanonical VARIANT_BOOL value.");
}

[[nodiscard]] constexpr VARIANT_BOOL nativeBoolean(const bool value) noexcept
{
    return value ? VARIANT_TRUE : VARIANT_FALSE;
}

template <typename Interface, typename Getter>
[[nodiscard]] Domain::Result<UniqueComInterface<Interface>> requiredInterface(
    Getter&& getter,
    const std::string_view action)
{
    UniqueComInterface<Interface> value;
    const HRESULT result = getter(value.put());
    auto exact = exactComResult(result, action);
    if (!exact) {
        return Domain::Result<UniqueComInterface<Interface>>::failure(
            std::move(exact).error());
    }
    if (!value) {
        return failure<UniqueComInterface<Interface>>(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{action} + " returned a null COM interface.");
    }
    return Domain::Result<UniqueComInterface<Interface>>::success(
        std::move(value));
}

template <typename Interface, typename Source>
[[nodiscard]] Domain::Result<UniqueComInterface<Interface>> requiredQuery(
    Source* source,
    const std::string_view action)
{
    UniqueComInterface<Interface> value;
    const HRESULT result = queryComInterface(source, value);
    auto exact = exactComResult(
        result,
        action,
        result == E_NOINTERFACE
            ? Domain::ErrorCodes::HostCapabilityUnavailable
            : Domain::ErrorCodes::InternalFailure);
    if (!exact) {
        return Domain::Result<UniqueComInterface<Interface>>::failure(
            std::move(exact).error());
    }
    if (!value) {
        return failure<UniqueComInterface<Interface>>(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{action} + " returned a null COM interface.");
    }
    return Domain::Result<UniqueComInterface<Interface>>::success(
        std::move(value));
}

template <typename Getter>
[[nodiscard]] Domain::Result<std::wstring> readBoundedWideText(
    Getter&& getter,
    const std::size_t maximumUtf16Units,
    const std::string_view action)
{
    UniqueBstr value;
    const HRESULT result = getter(value.put());
    auto exact = exactComResult(result, action);
    if (!exact) {
        return Domain::Result<std::wstring>::failure(
            std::move(exact).error());
    }
    const std::wstring_view view = value.view();
    if (view.size() > maximumUtf16Units) {
        return failure<std::wstring>(
            Domain::ErrorCodes::LimitExceeded,
            std::string{action} + " exceeded its UTF-16 bound.");
    }
    if (view.find(L'\0') != std::wstring_view::npos) {
        return failure<std::wstring>(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{action} + " contained an embedded NUL.");
    }
    return Domain::Result<std::wstring>::success(std::wstring{view});
}

template <typename Getter>
[[nodiscard]] Domain::Result<std::string> readBoundedText(
    Getter&& getter,
    const std::size_t maximumUtf16Units,
    const std::size_t maximumUtf8Bytes,
    const std::string_view action)
{
    auto native = readBoundedWideText(
        std::forward<Getter>(getter), maximumUtf16Units, action);
    if (!native) {
        return Domain::Result<std::string>::failure(
            std::move(native).error());
    }
    auto converted = strictUtf16ToUtf8(native.value());
    if (!converted) {
        return Domain::Result<std::string>::failure(
            std::move(converted).error());
    }
    if (converted.value().size() > maximumUtf8Bytes) {
        return failure<std::string>(
            Domain::ErrorCodes::LimitExceeded,
            std::string{action} + " exceeded its UTF-8 bound.");
    }
    return converted;
}

template <typename Getter>
[[nodiscard]] Domain::Result<std::string> readTaskText(
    Getter&& getter,
    const std::string_view action)
{
    return readBoundedText(
        std::forward<Getter>(getter),
        MaximumNativeTextUtf16Units,
        Manager::ManagerStartupTaskPolicy::MaximumTextBytes,
        action);
}

template <typename Getter>
[[nodiscard]] Domain::Result<std::optional<std::string>> readOptionalTaskText(
    Getter&& getter,
    const std::string_view action)
{
    auto value = readTaskText(std::forward<Getter>(getter), action);
    if (!value) {
        return Domain::Result<std::optional<std::string>>::failure(
            std::move(value).error());
    }
    if (value.value().empty()) {
        return Domain::Result<std::optional<std::string>>::success(
            std::nullopt);
    }
    return Domain::Result<std::optional<std::string>>::success(
        std::move(value).value());
}

struct ObservedDuration final {
    Manager::ManagerStartupTaskDuration value;
    bool nativeTextEmpty{};
};

template <typename Getter>
[[nodiscard]] Domain::Result<ObservedDuration> readDuration(
    Getter&& getter,
    const std::chrono::seconds emptyDefault,
    const std::string_view action)
{
    auto native = readBoundedWideText(
        std::forward<Getter>(getter),
        TaskSchedulerDurationCodec::MaximumTextUtf16Units,
        action);
    if (!native) {
        return Domain::Result<ObservedDuration>::failure(
            std::move(native).error());
    }
    if (native.value().empty()) {
        return Domain::Result<ObservedDuration>::success(
            ObservedDuration{
                Manager::ManagerStartupTaskDuration{emptyDefault}, true});
    }
    auto parsed = TaskSchedulerDurationCodec::parse(native.value());
    if (!parsed) {
        return Domain::Result<ObservedDuration>::failure(
            std::move(parsed).error());
    }
    return Domain::Result<ObservedDuration>::success(
        ObservedDuration{std::move(parsed).value(), false});
}

[[nodiscard]] Domain::Result<UniqueBstr> boundedBstr(
    const std::wstring_view value,
    const std::size_t maximumUtf16Units,
    const std::string_view label)
{
    if (value.size() > maximumUtf16Units) {
        return failure<UniqueBstr>(
            Domain::ErrorCodes::LimitExceeded,
            std::string{label} + " exceeds its UTF-16 bound.");
    }
    if (value.find(L'\0') != std::wstring_view::npos) {
        return failure<UniqueBstr>(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " contains an embedded NUL.");
    }
    auto result = UniqueBstr::copy(value);
    if (!result) {
        return failure<UniqueBstr>(
            Domain::ErrorCodes::InternalFailure,
            std::string{label} + " could not allocate a BSTR.");
    }
    return Domain::Result<UniqueBstr>::success(std::move(result));
}

[[nodiscard]] Domain::Result<UniqueBstr> taskTextBstr(
    const std::string_view value,
    const std::string_view label)
{
    if (value.size() > Manager::ManagerStartupTaskPolicy::MaximumTextBytes) {
        return failure<UniqueBstr>(
            Domain::ErrorCodes::LimitExceeded,
            std::string{label} + " exceeds its UTF-8 bound.");
    }
    auto converted = strictUtf8ToUtf16(value);
    if (!converted) {
        return Domain::Result<UniqueBstr>::failure(
            std::move(converted).error());
    }
    return boundedBstr(
        converted.value(), MaximumNativeTextUtf16Units, label);
}

template <typename Setter>
[[nodiscard]] Domain::Result<void> writeTaskText(
    const std::string_view value,
    Setter&& setter,
    const std::string_view action)
{
    auto native = taskTextBstr(value, action);
    if (!native) {
        return Domain::Result<void>::failure(std::move(native).error());
    }
    return exactComResult(setter(native.value().get()), action);
}

template <typename Setter>
[[nodiscard]] Domain::Result<void> writeOptionalTaskText(
    const std::optional<std::string>& value,
    Setter&& setter,
    const std::string_view action)
{
    return writeTaskText(
        value.has_value() ? std::string_view{*value} : std::string_view{},
        std::forward<Setter>(setter),
        action);
}

template <typename Setter>
[[nodiscard]] Domain::Result<void> writeDuration(
    const Manager::ManagerStartupTaskDuration& value,
    Setter&& setter,
    const std::string_view action)
{
    auto formatted = TaskSchedulerDurationCodec::format(value);
    if (!formatted) {
        return Domain::Result<void>::failure(
            std::move(formatted).error());
    }
    auto native = boundedBstr(
        formatted.value(),
        TaskSchedulerDurationCodec::MaximumTextUtf16Units,
        action);
    if (!native) {
        return Domain::Result<void>::failure(std::move(native).error());
    }
    return exactComResult(setter(native.value().get()), action);
}

struct SchedulerSession final {
    UniqueComInterface<ITaskService> service;
    UniqueComInterface<ITaskFolder> root;
};

[[nodiscard]] Domain::Result<SchedulerSession> connectScheduler(
    const Domain::OperationContext& context)
{
    auto checkpoint = contextCheckpoint(
        context, "connect to the current-user Task Scheduler");
    if (!checkpoint) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(checkpoint).error());
    }

    SchedulerSession session;
    const HRESULT created = ::CoCreateInstance(
        CLSID_TaskScheduler,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITaskService,
        reinterpret_cast<void**>(session.service.put()));
    auto exact = exactComResult(
        created, "create the Task Scheduler service");
    if (!exact) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(exact).error());
    }
    if (!session.service) {
        return failure<SchedulerSession>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned a null service interface.");
    }

    UniqueVariant server;
    UniqueVariant user;
    UniqueVariant domain;
    UniqueVariant password;
    exact = exactComResult(
        session.service->Connect(
            server.get(), user.get(), domain.get(), password.get()),
        "connect to the local current-user Task Scheduler");
    if (!exact) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(exact).error());
    }
    checkpoint = contextCheckpoint(
        context, "open the Task Scheduler root folder");
    if (!checkpoint) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(checkpoint).error());
    }

    auto rootPath = boundedBstr(L"\\", 1U, "The Task Scheduler root path");
    if (!rootPath) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(rootPath).error());
    }
    exact = exactComResult(
        session.service->GetFolder(rootPath.value().get(), session.root.put()),
        "open the Task Scheduler root folder");
    if (!exact) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(exact).error());
    }
    if (!session.root) {
        return failure<SchedulerSession>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned a null root folder interface.");
    }
    checkpoint = contextCheckpoint(
        context, "finish connecting to Task Scheduler");
    if (!checkpoint) {
        return Domain::Result<SchedulerSession>::failure(
            std::move(checkpoint).error());
    }
    return Domain::Result<SchedulerSession>::success(std::move(session));
}

[[nodiscard]] bool isMissingTaskResult(const HRESULT result) noexcept
{
    return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

[[nodiscard]] Domain::Result<UniqueBstr> taskPathBstr(
    const ManagerStartupResolvedRegistration& registration)
{
    auto taskPath = boundedBstr(
        registration.taskPath,
        Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units,
        "The resolved Manager startup task path");
    if (!taskPath) {
        return taskPath;
    }
    auto ownershipUri = strictUtf16ToUtf8(registration.taskPath);
    if (!ownershipUri) {
        return Domain::Result<UniqueBstr>::failure(
            std::move(ownershipUri).error());
    }
    if (ownershipUri.value() != registration.definition.ownership.uri) {
        return failure<UniqueBstr>(
            Domain::ErrorCodes::IntegrityFailure,
            "The Manager startup lookup path and ownership URI disagree.");
    }
    return taskPath;
}

[[nodiscard]] Domain::Result<std::optional<Domain::UtcTimePoint>> lastRunTime(
    const DATE value)
{
    if (value == 0.0) {
        return Domain::Result<std::optional<Domain::UtcTimePoint>>::success(
            std::nullopt);
    }
    if (!std::isfinite(value)) {
        return failure<std::optional<Domain::UtcTimePoint>>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned a nonfinite last-run time.");
    }

    SYSTEMTIME localTime{};
    if (::VariantTimeToSystemTime(value, &localTime) == 0) {
        return failure<std::optional<Domain::UtcTimePoint>>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned an invalid last-run DATE value.");
    }
    SYSTEMTIME utcTime{};
    if (::TzSpecificLocalTimeToSystemTime(
            nullptr, &localTime, &utcTime) == FALSE) {
        return Domain::Result<std::optional<Domain::UtcTimePoint>>::failure(
            makeWin32Error(
                "convert the Task Scheduler last-run time to UTC",
                ::GetLastError(),
                Domain::ErrorCodes::IntegrityFailure));
    }
    FILETIME fileTime{};
    if (::SystemTimeToFileTime(&utcTime, &fileTime) == FALSE) {
        return Domain::Result<std::optional<Domain::UtcTimePoint>>::failure(
            makeWin32Error(
                "convert the Task Scheduler last-run time to FILETIME",
                ::GetLastError(),
                Domain::ErrorCodes::IntegrityFailure));
    }

    constexpr std::uint64_t WindowsToUnixEpochTicks =
        116'444'736'000'000'000ULL;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32U) |
        fileTime.dwLowDateTime;
    std::int64_t signedTicks{};
    if (ticks >= WindowsToUnixEpochTicks) {
        const std::uint64_t elapsed = ticks - WindowsToUnixEpochTicks;
        if (elapsed >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return failure<std::optional<Domain::UtcTimePoint>>(
                Domain::ErrorCodes::LimitExceeded,
                "The Task Scheduler last-run time exceeds the system clock range.");
        }
        signedTicks = static_cast<std::int64_t>(elapsed);
    } else {
        const std::uint64_t elapsed = WindowsToUnixEpochTicks - ticks;
        if (elapsed >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) +
                1ULL) {
            return failure<std::optional<Domain::UtcTimePoint>>(
                Domain::ErrorCodes::LimitExceeded,
                "The Task Scheduler last-run time precedes the system clock range.");
        }
        signedTicks = elapsed ==
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)()) +
                    1ULL
            ? (std::numeric_limits<std::int64_t>::min)()
            : -static_cast<std::int64_t>(elapsed);
    }

    using HundredNanoseconds =
        std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    const HundredNanoseconds elapsed{signedTicks};
    const long double elapsedSeconds =
        std::chrono::duration<long double>{elapsed}.count();
    const long double minimumSeconds =
        std::chrono::duration<long double>{
            Domain::UtcTimePoint::duration::min()}
            .count();
    const long double maximumSeconds =
        std::chrono::duration<long double>{
            Domain::UtcTimePoint::duration::max()}
            .count();
    if (elapsedSeconds < minimumSeconds || elapsedSeconds > maximumSeconds) {
        return failure<std::optional<Domain::UtcTimePoint>>(
            Domain::ErrorCodes::LimitExceeded,
            "The Task Scheduler last-run time is outside the system clock range.");
    }
    return Domain::Result<std::optional<Domain::UtcTimePoint>>::success(
        Domain::UtcTimePoint{
            std::chrono::duration_cast<Domain::UtcTimePoint::duration>(elapsed)});
}

template <typename Getter>
[[nodiscard]] Domain::Result<bool> readBoolean(
    Getter&& getter,
    const std::string_view action)
{
    VARIANT_BOOL native{};
    auto exact = exactComResult(getter(&native), action);
    if (!exact) {
        return Domain::Result<bool>::failure(std::move(exact).error());
    }
    return observedBoolean(native, action);
}

[[nodiscard]] Domain::Result<std::optional<Domain::PathText>> observedPath(
    std::string value,
    const std::string_view label)
{
    if (value.empty()) {
        return Domain::Result<std::optional<Domain::PathText>>::success(
            std::nullopt);
    }
    auto path = Domain::PathText::create(value);
    if (!path) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{label} +
                    " is not representable as bounded path text."));
    }
    return Domain::Result<std::optional<Domain::PathText>>::success(
        std::move(path).value());
}

[[nodiscard]] Domain::Result<Manager::ManagerStartupTaskOwnership>
projectOwnership(ITaskDefinition* definition)
{
    auto registrationInfo = requiredInterface<IRegistrationInfo>(
        [definition](IRegistrationInfo** value) {
            return definition->get_RegistrationInfo(value);
        },
        "read Task Scheduler registration information");
    if (!registrationInfo) {
        return Domain::Result<Manager::ManagerStartupTaskOwnership>::failure(
            std::move(registrationInfo).error());
    }

    Manager::ManagerStartupTaskOwnership ownership;
    auto source = readTaskText(
        [&registrationInfo](BSTR* value) {
            return registrationInfo.value()->get_Source(value);
        },
        "read the Task Scheduler registration source");
    if (!source) {
        return Domain::Result<Manager::ManagerStartupTaskOwnership>::failure(
            std::move(source).error());
    }
    ownership.source = std::move(source).value();

    auto uri = readTaskText(
        [&registrationInfo](BSTR* value) {
            return registrationInfo.value()->get_URI(value);
        },
        "read the Task Scheduler registration URI");
    if (!uri) {
        return Domain::Result<Manager::ManagerStartupTaskOwnership>::failure(
            std::move(uri).error());
    }
    ownership.uri = std::move(uri).value();
    return Domain::Result<Manager::ManagerStartupTaskOwnership>::success(
        std::move(ownership));
}

[[nodiscard]] Domain::Result<Manager::ManagerStartupTaskPrincipal>
projectPrincipal(
    ITaskDefinition* definition,
    const WindowsCurrentUserIdentity& currentIdentity,
    const Domain::OperationContext& context)
{
    auto principal = requiredInterface<IPrincipal>(
        [definition](IPrincipal** value) {
            return definition->get_Principal(value);
        },
        "read the Task Scheduler principal");
    if (!principal) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(principal).error());
    }

    Manager::ManagerStartupTaskPrincipal projected;
    auto id = readTaskText(
        [&principal](BSTR* value) {
            return principal.value()->get_Id(value);
        },
        "read the Task Scheduler principal ID");
    if (!id) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(id).error());
    }
    projected.id = std::move(id).value();

    auto displayName = readTaskText(
        [&principal](BSTR* value) {
            return principal.value()->get_DisplayName(value);
        },
        "read the Task Scheduler principal display name");
    if (!displayName) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(displayName).error());
    }
    projected.displayName = std::move(displayName).value();

    auto user = readTaskText(
        [&principal](BSTR* value) {
            return principal.value()->get_UserId(value);
        },
        "read the Task Scheduler principal user ID");
    if (!user) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(user).error());
    }
    auto normalizedUser = normalizeCurrentUserIdentity(
        std::move(user).value(),
        currentIdentity,
        context,
        "The Task Scheduler principal user ID");
    if (!normalizedUser) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(normalizedUser).error());
    }
    projected.userIdentity = std::move(normalizedUser).value();

    auto group = readTaskText(
        [&principal](BSTR* value) {
            return principal.value()->get_GroupId(value);
        },
        "read the Task Scheduler principal group ID");
    if (!group) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(group).error());
    }
    projected.groupIdentity = std::move(group).value();

    TASK_LOGON_TYPE logonType{};
    auto exact = exactComResult(
        principal.value()->get_LogonType(&logonType),
        "read the Task Scheduler principal logon type");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(exact).error());
    }
    projected.logonType = logonType == TASK_LOGON_INTERACTIVE_TOKEN
        ? Manager::ManagerStartupTaskLogonType::CurrentInteractiveUser
        : Manager::ManagerStartupTaskLogonType::Other;

    TASK_RUNLEVEL_TYPE runLevel{};
    exact = exactComResult(
        principal.value()->get_RunLevel(&runLevel),
        "read the Task Scheduler principal run level");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(exact).error());
    }
    projected.runLevel = runLevel == TASK_RUNLEVEL_LUA
        ? Manager::ManagerStartupTaskRunLevel::LeastPrivilege
        : Manager::ManagerStartupTaskRunLevel::Other;

    auto principal2 = requiredQuery<IPrincipal2>(
        principal.value().get(),
        "open Task Scheduler principal version 2");
    if (!principal2) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(principal2).error());
    }

    TASK_PROCESSTOKENSID_TYPE sidType{};
    exact = exactComResult(
        principal2.value()->get_ProcessTokenSidType(&sidType),
        "read the Task Scheduler process-token SID type");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(exact).error());
    }
    projected.processTokenSidType = sidType == TASK_PROCESSTOKENSID_DEFAULT
        ? Manager::ManagerStartupTaskProcessTokenSidType::Default
        : Manager::ManagerStartupTaskProcessTokenSidType::Other;

    long privilegeCount{};
    exact = exactComResult(
        principal2.value()->get_RequiredPrivilegeCount(&privilegeCount),
        "read the Task Scheduler required-privilege count");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
            std::move(exact).error());
    }
    if (privilegeCount < 0 ||
        static_cast<std::size_t>(privilegeCount) >
            Manager::ManagerStartupTaskPolicy::MaximumObservedPrivilegeCount) {
        return failure<Manager::ManagerStartupTaskPrincipal>(
            privilegeCount < 0
                ? Domain::ErrorCodes::IntegrityFailure
                : Domain::ErrorCodes::LimitExceeded,
            "Task Scheduler returned an invalid or over-limit required-privilege count.");
    }
    projected.requiredPrivileges.reserve(
        static_cast<std::size_t>(privilegeCount));
    for (long index = 1; index <= privilegeCount; ++index) {
        auto checkpoint = contextCheckpoint(
            context, "inspect a Task Scheduler required privilege");
        if (!checkpoint) {
            return Domain::Result<
                Manager::ManagerStartupTaskPrincipal>::failure(
                std::move(checkpoint).error());
        }
        auto privilege = readTaskText(
            [&principal2, index](BSTR* value) {
                return principal2.value()->get_RequiredPrivilege(index, value);
            },
            "read a Task Scheduler required privilege");
        if (!privilege) {
            return Domain::Result<Manager::ManagerStartupTaskPrincipal>::failure(
                std::move(privilege).error());
        }
        projected.requiredPrivileges.push_back(
            std::move(privilege).value());
    }
    return Domain::Result<Manager::ManagerStartupTaskPrincipal>::success(
        std::move(projected));
}

[[nodiscard]] Domain::Result<std::optional<Manager::ManagerStartupTaskRepetition>>
projectRepetition(ITrigger* trigger)
{
    auto repetition = requiredInterface<IRepetitionPattern>(
        [trigger](IRepetitionPattern** value) {
            return trigger->get_Repetition(value);
        },
        "read the Task Scheduler trigger repetition");
    if (!repetition) {
        return Domain::Result<
            std::optional<Manager::ManagerStartupTaskRepetition>>::failure(
            std::move(repetition).error());
    }

    auto interval = readDuration(
        [&repetition](BSTR* value) {
            return repetition.value()->get_Interval(value);
        },
        std::chrono::seconds::zero(),
        "read the Task Scheduler repetition interval");
    if (!interval) {
        return Domain::Result<
            std::optional<Manager::ManagerStartupTaskRepetition>>::failure(
            std::move(interval).error());
    }
    auto duration = readDuration(
        [&repetition](BSTR* value) {
            return repetition.value()->get_Duration(value);
        },
        std::chrono::seconds::zero(),
        "read the Task Scheduler repetition duration");
    if (!duration) {
        return Domain::Result<
            std::optional<Manager::ManagerStartupTaskRepetition>>::failure(
            std::move(duration).error());
    }
    auto stop = readBoolean(
        [&repetition](VARIANT_BOOL* value) {
            return repetition.value()->get_StopAtDurationEnd(value);
        },
        "read the Task Scheduler repetition stop policy");
    if (!stop) {
        return Domain::Result<
            std::optional<Manager::ManagerStartupTaskRepetition>>::failure(
            std::move(stop).error());
    }
    if (interval.value().nativeTextEmpty &&
        duration.value().nativeTextEmpty && !stop.value()) {
        return Domain::Result<
            std::optional<Manager::ManagerStartupTaskRepetition>>::success(
            std::nullopt);
    }
    return Domain::Result<
        std::optional<Manager::ManagerStartupTaskRepetition>>::success(
        Manager::ManagerStartupTaskRepetition{
            std::move(interval).value().value,
            std::move(duration).value().value,
            stop.value()});
}

[[nodiscard]] Domain::Result<Manager::ManagerStartupTaskTrigger>
projectTrigger(
    ITrigger* trigger,
    const WindowsCurrentUserIdentity& currentIdentity,
    const Domain::OperationContext& context)
{
    Manager::ManagerStartupTaskTrigger projected;
    TASK_TRIGGER_TYPE2 type{};
    auto exact = exactComResult(
        trigger->get_Type(&type),
        "read the Task Scheduler trigger type");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(exact).error());
    }
    projected.kind = type == TASK_TRIGGER_LOGON
        ? Manager::ManagerStartupTaskTriggerKind::UserLogon
        : Manager::ManagerStartupTaskTriggerKind::Other;

    auto id = readTaskText(
        [trigger](BSTR* value) { return trigger->get_Id(value); },
        "read the Task Scheduler trigger ID");
    if (!id) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(id).error());
    }
    projected.id = std::move(id).value();

    auto repetition = projectRepetition(trigger);
    if (!repetition) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(repetition).error());
    }
    projected.repetition = std::move(repetition).value();

    auto executionLimit = readDuration(
        [trigger](BSTR* value) {
            return trigger->get_ExecutionTimeLimit(value);
        },
        DefaultTaskAndTriggerExecutionTimeLimit,
        "read the Task Scheduler trigger execution limit");
    if (!executionLimit) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(executionLimit).error());
    }
    projected.executionTimeLimit =
        std::move(executionLimit).value().value;

    auto start = readOptionalTaskText(
        [trigger](BSTR* value) {
            return trigger->get_StartBoundary(value);
        },
        "read the Task Scheduler trigger start boundary");
    if (!start) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(start).error());
    }
    projected.startBoundary = std::move(start).value();

    auto end = readOptionalTaskText(
        [trigger](BSTR* value) {
            return trigger->get_EndBoundary(value);
        },
        "read the Task Scheduler trigger end boundary");
    if (!end) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(end).error());
    }
    projected.endBoundary = std::move(end).value();

    auto enabled = readBoolean(
        [trigger](VARIANT_BOOL* value) {
            return trigger->get_Enabled(value);
        },
        "read the Task Scheduler trigger enabled state");
    if (!enabled) {
        return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
            std::move(enabled).error());
    }
    projected.enabled = enabled.value();

    if (type == TASK_TRIGGER_LOGON) {
        auto logon = requiredQuery<ILogonTrigger>(
            trigger, "open the Task Scheduler logon trigger");
        if (!logon) {
            return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
                std::move(logon).error());
        }
        auto user = readTaskText(
            [&logon](BSTR* value) {
                return logon.value()->get_UserId(value);
            },
            "read the Task Scheduler logon-trigger user ID");
        if (!user) {
            return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
                std::move(user).error());
        }
        auto normalizedUser = normalizeCurrentUserIdentity(
            std::move(user).value(),
            currentIdentity,
            context,
            "The Task Scheduler logon-trigger user ID");
        if (!normalizedUser) {
            return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
                std::move(normalizedUser).error());
        }
        projected.userIdentity = std::move(normalizedUser).value();

        auto delay = readDuration(
            [&logon](BSTR* value) {
                return logon.value()->get_Delay(value);
            },
            std::chrono::seconds::zero(),
            "read the Task Scheduler logon-trigger delay");
        if (!delay) {
            return Domain::Result<Manager::ManagerStartupTaskTrigger>::failure(
                std::move(delay).error());
        }
        projected.delay = std::move(delay).value().value;
    }
    return Domain::Result<Manager::ManagerStartupTaskTrigger>::success(
        std::move(projected));
}

[[nodiscard]] Domain::Result<std::vector<Manager::ManagerStartupTaskTrigger>>
projectTriggers(
    ITaskDefinition* definition,
    const WindowsCurrentUserIdentity& currentIdentity,
    const Domain::OperationContext& context)
{
    auto collection = requiredInterface<ITriggerCollection>(
        [definition](ITriggerCollection** value) {
            return definition->get_Triggers(value);
        },
        "read the Task Scheduler trigger collection");
    if (!collection) {
        return Domain::Result<
            std::vector<Manager::ManagerStartupTaskTrigger>>::failure(
            std::move(collection).error());
    }
    long count{};
    auto exact = exactComResult(
        collection.value()->get_Count(&count),
        "read the Task Scheduler trigger count");
    if (!exact) {
        return Domain::Result<
            std::vector<Manager::ManagerStartupTaskTrigger>>::failure(
            std::move(exact).error());
    }
    if (count < 0 || static_cast<std::size_t>(count) >
            Manager::ManagerStartupTaskPolicy::MaximumObservedTriggerCount) {
        return failure<std::vector<Manager::ManagerStartupTaskTrigger>>(
            count < 0
                ? Domain::ErrorCodes::IntegrityFailure
                : Domain::ErrorCodes::LimitExceeded,
            "Task Scheduler returned an invalid or over-limit trigger count.");
    }

    std::vector<Manager::ManagerStartupTaskTrigger> projected;
    projected.reserve(static_cast<std::size_t>(count));
    for (long index = 1; index <= count; ++index) {
        auto checkpoint = contextCheckpoint(
            context, "inspect a Task Scheduler trigger");
        if (!checkpoint) {
            return Domain::Result<
                std::vector<Manager::ManagerStartupTaskTrigger>>::failure(
                std::move(checkpoint).error());
        }
        auto trigger = requiredInterface<ITrigger>(
            [&collection, index](ITrigger** value) {
                return collection.value()->get_Item(index, value);
            },
            "read a Task Scheduler trigger");
        if (!trigger) {
            return Domain::Result<
                std::vector<Manager::ManagerStartupTaskTrigger>>::failure(
                std::move(trigger).error());
        }
        auto item = projectTrigger(
            trigger.value().get(), currentIdentity, context);
        if (!item) {
            return Domain::Result<
                std::vector<Manager::ManagerStartupTaskTrigger>>::failure(
                std::move(item).error());
        }
        projected.push_back(std::move(item).value());
    }
    return Domain::Result<
        std::vector<Manager::ManagerStartupTaskTrigger>>::success(
        std::move(projected));
}

[[nodiscard]] Domain::Result<Manager::ManagerStartupTaskAction>
projectAction(IAction* action)
{
    Manager::ManagerStartupTaskAction projected;
    TASK_ACTION_TYPE type{};
    auto exact = exactComResult(
        action->get_Type(&type),
        "read the Task Scheduler action type");
    if (!exact) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(exact).error());
    }
    projected.kind = type == TASK_ACTION_EXEC
        ? Manager::ManagerStartupTaskActionKind::Execute
        : Manager::ManagerStartupTaskActionKind::Other;

    auto id = readTaskText(
        [action](BSTR* value) { return action->get_Id(value); },
        "read the Task Scheduler action ID");
    if (!id) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(id).error());
    }
    projected.id = std::move(id).value();

    if (type != TASK_ACTION_EXEC) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::success(
            std::move(projected));
    }

    auto executable = requiredQuery<IExecAction>(
        action, "open the Task Scheduler executable action");
    if (!executable) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(executable).error());
    }

    auto pathText = readTaskText(
        [&executable](BSTR* value) {
            return executable.value()->get_Path(value);
        },
        "read the Task Scheduler executable path");
    if (!pathText) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(pathText).error());
    }
    auto path = observedPath(
        std::move(pathText).value(),
        "The Task Scheduler executable path");
    if (!path) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(path).error());
    }
    projected.executable = std::move(path).value();

    auto arguments = readTaskText(
        [&executable](BSTR* value) {
            return executable.value()->get_Arguments(value);
        },
        "read the Task Scheduler executable arguments");
    if (!arguments) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(arguments).error());
    }
    projected.arguments = std::move(arguments).value();

    auto directoryText = readTaskText(
        [&executable](BSTR* value) {
            return executable.value()->get_WorkingDirectory(value);
        },
        "read the Task Scheduler executable working directory");
    if (!directoryText) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(directoryText).error());
    }
    auto directory = observedPath(
        std::move(directoryText).value(),
        "The Task Scheduler executable working directory");
    if (!directory) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(directory).error());
    }
    projected.workingDirectory = std::move(directory).value();

    auto executable2 = requiredQuery<IExecAction2>(
        executable.value().get(),
        "open Task Scheduler executable action version 2");
    if (!executable2) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(executable2).error());
    }
    auto hidden = readBoolean(
        [&executable2](VARIANT_BOOL* value) {
            return executable2.value()->get_HideAppWindow(value);
        },
        "read the Task Scheduler executable hidden-window policy");
    if (!hidden) {
        return Domain::Result<Manager::ManagerStartupTaskAction>::failure(
            std::move(hidden).error());
    }
    projected.hideAppWindow = hidden.value();
    return Domain::Result<Manager::ManagerStartupTaskAction>::success(
        std::move(projected));
}

struct ProjectedActions final {
    std::string context;
    std::vector<Manager::ManagerStartupTaskAction> values;
};

[[nodiscard]] Domain::Result<ProjectedActions> projectActions(
    ITaskDefinition* definition,
    const Domain::OperationContext& context)
{
    auto collection = requiredInterface<IActionCollection>(
        [definition](IActionCollection** value) {
            return definition->get_Actions(value);
        },
        "read the Task Scheduler action collection");
    if (!collection) {
        return Domain::Result<ProjectedActions>::failure(
            std::move(collection).error());
    }

    ProjectedActions projected;
    auto actionContext = readTaskText(
        [&collection](BSTR* value) {
            return collection.value()->get_Context(value);
        },
        "read the Task Scheduler action context");
    if (!actionContext) {
        return Domain::Result<ProjectedActions>::failure(
            std::move(actionContext).error());
    }
    projected.context = std::move(actionContext).value();

    long count{};
    auto exact = exactComResult(
        collection.value()->get_Count(&count),
        "read the Task Scheduler action count");
    if (!exact) {
        return Domain::Result<ProjectedActions>::failure(
            std::move(exact).error());
    }
    if (count < 0 || static_cast<std::size_t>(count) >
            Manager::ManagerStartupTaskPolicy::MaximumObservedActionCount) {
        return failure<ProjectedActions>(
            count < 0
                ? Domain::ErrorCodes::IntegrityFailure
                : Domain::ErrorCodes::LimitExceeded,
            "Task Scheduler returned an invalid or over-limit action count.");
    }

    projected.values.reserve(static_cast<std::size_t>(count));
    for (long index = 1; index <= count; ++index) {
        auto checkpoint = contextCheckpoint(
            context, "inspect a Task Scheduler action");
        if (!checkpoint) {
            return Domain::Result<ProjectedActions>::failure(
                std::move(checkpoint).error());
        }
        auto action = requiredInterface<IAction>(
            [&collection, index](IAction** value) {
                return collection.value()->get_Item(index, value);
            },
            "read a Task Scheduler action");
        if (!action) {
            return Domain::Result<ProjectedActions>::failure(
                std::move(action).error());
        }
        auto item = projectAction(action.value().get());
        if (!item) {
            return Domain::Result<ProjectedActions>::failure(
                std::move(item).error());
        }
        projected.values.push_back(std::move(item).value());
    }
    return Domain::Result<ProjectedActions>::success(std::move(projected));
}

struct ProjectedSettings final {
    Manager::ManagerStartupTaskSettings value;
    bool taskDefinitionEnabled{};
};

[[nodiscard]] Domain::Result<ProjectedSettings> projectSettings(
    ITaskDefinition* definition)
{
    auto settings = requiredInterface<ITaskSettings>(
        [definition](ITaskSettings** value) {
            return definition->get_Settings(value);
        },
        "read the Task Scheduler settings");
    if (!settings) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(settings).error());
    }

    ProjectedSettings projected;
    auto boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_AllowDemandStart(value);
        },
        "read the Task Scheduler demand-start policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.allowDemandStart = boolean.value();

    auto restartInterval = readDuration(
        [&settings](BSTR* value) {
            return settings.value()->get_RestartInterval(value);
        },
        std::chrono::seconds::zero(),
        "read the Task Scheduler restart interval");
    if (!restartInterval) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(restartInterval).error());
    }
    projected.value.restartInterval =
        std::move(restartInterval).value().value;

    int restartCount{};
    auto exact = exactComResult(
        settings.value()->get_RestartCount(&restartCount),
        "read the Task Scheduler restart count");
    if (!exact) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(exact).error());
    }
    if (restartCount < 0 || restartCount >
            static_cast<int>((std::numeric_limits<std::uint8_t>::max)())) {
        return failure<ProjectedSettings>(
            restartCount < 0
                ? Domain::ErrorCodes::IntegrityFailure
                : Domain::ErrorCodes::LimitExceeded,
            "Task Scheduler returned an unrepresentable restart count.");
    }
    projected.value.restartCount = static_cast<std::uint8_t>(restartCount);

    TASK_INSTANCES_POLICY instances{};
    exact = exactComResult(
        settings.value()->get_MultipleInstances(&instances),
        "read the Task Scheduler multiple-instance policy");
    if (!exact) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(exact).error());
    }
    projected.value.multipleInstances = instances == TASK_INSTANCES_IGNORE_NEW
        ? Manager::ManagerStartupTaskMultipleInstances::IgnoreNew
        : Manager::ManagerStartupTaskMultipleInstances::Other;

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_StopIfGoingOnBatteries(value);
        },
        "read the Task Scheduler stop-on-battery policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.stopIfGoingOnBatteries = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_DisallowStartIfOnBatteries(value);
        },
        "read the Task Scheduler battery-start policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.disallowStartIfOnBatteries = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_AllowHardTerminate(value);
        },
        "read the Task Scheduler hard-termination policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.allowHardTerminate = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_StartWhenAvailable(value);
        },
        "read the Task Scheduler missed-start policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.startWhenAvailable = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_RunOnlyIfNetworkAvailable(value);
        },
        "read the Task Scheduler network gate");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.runOnlyIfNetworkAvailable = boolean.value();

    auto executionLimit = readDuration(
        [&settings](BSTR* value) {
            return settings.value()->get_ExecutionTimeLimit(value);
        },
        DefaultTaskAndTriggerExecutionTimeLimit,
        "read the Task Scheduler execution limit");
    if (!executionLimit) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(executionLimit).error());
    }
    projected.value.executionTimeLimit =
        std::move(executionLimit).value().value;

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_Enabled(value);
        },
        "read the task-definition enabled state");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.taskDefinitionEnabled = boolean.value();

    auto expiration = readDuration(
        [&settings](BSTR* value) {
            return settings.value()->get_DeleteExpiredTaskAfter(value);
        },
        std::chrono::seconds::zero(),
        "read the Task Scheduler expiration delay");
    if (!expiration) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(expiration).error());
    }
    projected.value.deleteExpiredTaskAfter =
        std::move(expiration).value().value;

    int priority{};
    exact = exactComResult(
        settings.value()->get_Priority(&priority),
        "read the Task Scheduler priority");
    if (!exact) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(exact).error());
    }
    if (priority < 0 || priority >
            static_cast<int>((std::numeric_limits<std::uint8_t>::max)())) {
        return failure<ProjectedSettings>(
            priority < 0
                ? Domain::ErrorCodes::IntegrityFailure
                : Domain::ErrorCodes::LimitExceeded,
            "Task Scheduler returned an unrepresentable priority.");
    }
    projected.value.priority = static_cast<std::uint8_t>(priority);

    TASK_COMPATIBILITY compatibility{};
    exact = exactComResult(
        settings.value()->get_Compatibility(&compatibility),
        "read the Task Scheduler compatibility level");
    if (!exact) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(exact).error());
    }
    projected.value.compatibility = compatibility == TASK_COMPATIBILITY_V2_4
        ? Manager::ManagerStartupTaskCompatibility::Windows10OrLater
        : Manager::ManagerStartupTaskCompatibility::Other;

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_Hidden(value);
        },
        "read the Task Scheduler hidden-task policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.hidden = boolean.value();

    auto idle = requiredInterface<IIdleSettings>(
        [&settings](IIdleSettings** value) {
            return settings.value()->get_IdleSettings(value);
        },
        "read the Task Scheduler idle settings");
    if (!idle) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(idle).error());
    }
    auto idleDuration = readDuration(
        [&idle](BSTR* value) {
            return idle.value()->get_IdleDuration(value);
        },
        std::chrono::minutes{10},
        "read the Task Scheduler idle duration");
    if (!idleDuration) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(idleDuration).error());
    }
    projected.value.idleDuration = std::move(idleDuration).value().value;

    auto idleWait = readDuration(
        [&idle](BSTR* value) {
            return idle.value()->get_WaitTimeout(value);
        },
        std::chrono::hours{1},
        "read the Task Scheduler idle wait timeout");
    if (!idleWait) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(idleWait).error());
    }
    projected.value.idleWaitTimeout = std::move(idleWait).value().value;

    boolean = readBoolean(
        [&idle](VARIANT_BOOL* value) {
            return idle.value()->get_StopOnIdleEnd(value);
        },
        "read the Task Scheduler stop-on-idle-end policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.stopOnIdleEnd = boolean.value();

    boolean = readBoolean(
        [&idle](VARIANT_BOOL* value) {
            return idle.value()->get_RestartOnIdle(value);
        },
        "read the Task Scheduler restart-on-idle policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.restartOnIdle = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_RunOnlyIfIdle(value);
        },
        "read the Task Scheduler idle gate");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.runOnlyIfIdle = boolean.value();

    boolean = readBoolean(
        [&settings](VARIANT_BOOL* value) {
            return settings.value()->get_WakeToRun(value);
        },
        "read the Task Scheduler wake-to-run policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.wakeToRun = boolean.value();

    auto network = requiredInterface<INetworkSettings>(
        [&settings](INetworkSettings** value) {
            return settings.value()->get_NetworkSettings(value);
        },
        "read the Task Scheduler network settings");
    if (!network) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(network).error());
    }
    auto networkName = readTaskText(
        [&network](BSTR* value) {
            return network.value()->get_Name(value);
        },
        "read the Task Scheduler network profile name");
    if (!networkName) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(networkName).error());
    }
    projected.value.networkProfileName = std::move(networkName).value();

    auto networkId = readTaskText(
        [&network](BSTR* value) {
            return network.value()->get_Id(value);
        },
        "read the Task Scheduler network profile ID");
    if (!networkId) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(networkId).error());
    }
    projected.value.networkProfileId = std::move(networkId).value();

    auto settings2 = requiredQuery<ITaskSettings2>(
        settings.value().get(),
        "open Task Scheduler settings version 2");
    if (!settings2) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(settings2).error());
    }
    boolean = readBoolean(
        [&settings2](VARIANT_BOOL* value) {
            return settings2.value()->get_DisallowStartOnRemoteAppSession(value);
        },
        "read the Task Scheduler RemoteApp-session policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.disallowStartOnRemoteAppSession = boolean.value();

    boolean = readBoolean(
        [&settings2](VARIANT_BOOL* value) {
            return settings2.value()->get_UseUnifiedSchedulingEngine(value);
        },
        "read the Task Scheduler unified-engine policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.useUnifiedSchedulingEngine = boolean.value();

    auto settings3 = requiredQuery<ITaskSettings3>(
        settings.value().get(),
        "open Task Scheduler settings version 3");
    if (!settings3) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(settings3).error());
    }
    UniqueComInterface<IMaintenanceSettings> maintenance;
    exact = exactComResult(
        settings3.value()->get_MaintenanceSettings(maintenance.put()),
        "read the Task Scheduler maintenance settings");
    if (!exact) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(exact).error());
    }
    projected.value.maintenanceSettingsPresent = static_cast<bool>(maintenance);

    boolean = readBoolean(
        [&settings3](VARIANT_BOOL* value) {
            return settings3.value()->get_Volatile(value);
        },
        "read the Task Scheduler volatile-task policy");
    if (!boolean) {
        return Domain::Result<ProjectedSettings>::failure(
            std::move(boolean).error());
    }
    projected.value.volatileTask = boolean.value();
    return Domain::Result<ProjectedSettings>::success(std::move(projected));
}

[[nodiscard]] Domain::Result<Manager::ManagerStartupTaskObservation>
projectRegisteredTask(
    IRegisteredTask* task,
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context)
{
    auto checkpoint = contextCheckpoint(
        context, "inspect the registered Manager startup task");
    if (!checkpoint) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(checkpoint).error());
    }

    auto identity = readBoundedText(
        [task](BSTR* value) { return task->get_Path(value); },
        Domain::MaximumManagerStartupRegistrationIdentityBytes,
        Domain::MaximumManagerStartupRegistrationIdentityBytes,
        "read the registered Manager startup task path");
    if (!identity) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(identity).error());
    }
    if (identity.value().empty()) {
        return failure<Manager::ManagerStartupTaskObservation>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned an empty registered-task path.");
    }
    auto definition = requiredInterface<ITaskDefinition>(
        [task](ITaskDefinition** value) {
            return task->get_Definition(value);
        },
        "read the registered Manager startup task definition");
    if (!definition) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(definition).error());
    }

    auto ownership = projectOwnership(definition.value().get());
    if (!ownership) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(ownership).error());
    }
    auto ownershipProjection = ManagerStartupOwnershipProjector::project(
        std::move(identity).value(),
        registration.definition.ownership,
        std::move(ownership).value());
    if (ownershipProjection.foreign) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::success(
            std::move(ownershipProjection.observation));
    }

    auto observation = std::move(ownershipProjection.observation);
    Manager::ManagerStartupTaskDefinition projected =
        std::move(*observation.definition);
    observation.definition.reset();

    auto registeredEnabled = readBoolean(
        [task](VARIANT_BOOL* value) {
            return task->get_Enabled(value);
        },
        "read the registered Manager startup task enabled state");
    if (!registeredEnabled) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(registeredEnabled).error());
    }
    observation.enabled = registeredEnabled.value();

    TASK_STATE state{};
    auto exact = exactComResult(
        task->get_State(&state),
        "read the registered Manager startup task state");
    if (!exact) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(exact).error());
    }
    observation.running = state == TASK_STATE_RUNNING;

    LONG lastResult{};
    const HRESULT lastResultStatus = task->get_LastTaskResult(&lastResult);
    if (lastResultStatus != SCHED_S_TASK_HAS_NOT_RUN) {
        exact = exactComResult(
            lastResultStatus,
            "read the registered Manager startup task last result");
        if (!exact) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                std::move(exact).error());
        }
        static_assert(sizeof(LONG) == sizeof(std::int32_t));
        observation.lastResult = static_cast<std::int32_t>(lastResult);
    }

    DATE lastRun{};
    const HRESULT lastRunStatus = task->get_LastRunTime(&lastRun);
    if (lastRunStatus != SCHED_S_TASK_HAS_NOT_RUN) {
        exact = exactComResult(
            lastRunStatus,
            "read the registered Manager startup task last-run time");
        if (!exact) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                std::move(exact).error());
        }
        auto timestamp = lastRunTime(lastRun);
        if (!timestamp) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                std::move(timestamp).error());
        }
        observation.lastRunAt = std::move(timestamp).value();
    }

    auto currentIdentity = WindowsCurrentUserIdentity::load();
    if (!currentIdentity) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(currentIdentity).error());
    }
    if (registration.definition.principal.userIdentity !=
        currentIdentity.value().sidText()) {
        return failure<Manager::ManagerStartupTaskObservation>(
            Domain::ErrorCodes::IntegrityFailure,
            "The resolved startup registration no longer matches the current-user SID.");
    }

    auto principal = projectPrincipal(
        definition.value().get(), currentIdentity.value(), context);
    if (!principal) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(principal).error());
    }
    projected.principal = std::move(principal).value();

    auto triggers = projectTriggers(
        definition.value().get(), currentIdentity.value(), context);
    if (!triggers) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(triggers).error());
    }
    projected.triggers = std::move(triggers).value();

    auto actions = projectActions(definition.value().get(), context);
    if (!actions) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(actions).error());
    }
    auto projectedActions = std::move(actions).value();
    projected.actionContext = std::move(projectedActions.context);
    projected.actions = std::move(projectedActions.values);

    auto settings = projectSettings(definition.value().get());
    if (!settings) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(settings).error());
    }
    if (settings.value().taskDefinitionEnabled != observation.enabled) {
        return failure<Manager::ManagerStartupTaskObservation>(
            Domain::ErrorCodes::IntegrityFailure,
            "The registered-task and task-definition enabled states disagree.");
    }
    projected.settings = std::move(settings).value().value;

    checkpoint = contextCheckpoint(
        context, "finish projecting the Manager startup task");
    if (!checkpoint) {
        return Domain::Result<
            Manager::ManagerStartupTaskObservation>::failure(
            std::move(checkpoint).error());
    }
    observation.definition = std::move(projected);
    observation.launchProjectionComplete = true;
    return Domain::Result<Manager::ManagerStartupTaskObservation>::success(
        std::move(observation));
}

struct RegisteredTaskLookup final {
    UniqueComInterface<IRegisteredTask> task;
    bool missing{};
};

[[nodiscard]] Domain::Result<RegisteredTaskLookup> lookupRegisteredTask(
    ITaskFolder* root,
    const ManagerStartupResolvedRegistration& registration)
{
    auto taskPath = taskPathBstr(registration);
    if (!taskPath) {
        return Domain::Result<RegisteredTaskLookup>::failure(
            std::move(taskPath).error());
    }
    RegisteredTaskLookup lookup;
    const HRESULT result = root->GetTask(
        taskPath.value().get(), lookup.task.put());
    if (isMissingTaskResult(result)) {
        return Domain::Result<RegisteredTaskLookup>::success(
            RegisteredTaskLookup{{}, true});
    }
    auto exact = exactComResult(
        result, "open the registered Manager startup task");
    if (!exact) {
        return Domain::Result<RegisteredTaskLookup>::failure(
            std::move(exact).error());
    }
    if (!lookup.task) {
        return failure<RegisteredTaskLookup>(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned a null registered-task interface.");
    }
    return Domain::Result<RegisteredTaskLookup>::success(std::move(lookup));
}

[[nodiscard]] Domain::Result<void> configureOwnership(
    ITaskDefinition* definition,
    const Manager::ManagerStartupTaskOwnership& ownership)
{
    auto info = requiredInterface<IRegistrationInfo>(
        [definition](IRegistrationInfo** value) {
            return definition->get_RegistrationInfo(value);
        },
        "open Task Scheduler registration information for writing");
    if (!info) {
        return Domain::Result<void>::failure(std::move(info).error());
    }
    auto written = writeTaskText(
        ownership.source,
        [&info](BSTR value) {
            return info.value()->put_Source(value);
        },
        "write the Task Scheduler registration source");
    if (!written) {
        return written;
    }
    return writeTaskText(
        ownership.uri,
        [&info](BSTR value) {
            return info.value()->put_URI(value);
        },
        "write the Task Scheduler registration URI");
}

[[nodiscard]] Domain::Result<void> configurePrincipal(
    ITaskDefinition* definition,
    const Manager::ManagerStartupTaskPrincipal& expected,
    const Domain::OperationContext& context)
{
    auto principal = requiredInterface<IPrincipal>(
        [definition](IPrincipal** value) {
            return definition->get_Principal(value);
        },
        "open the Task Scheduler principal for writing");
    if (!principal) {
        return Domain::Result<void>::failure(
            std::move(principal).error());
    }

    auto written = writeTaskText(
        expected.id,
        [&principal](BSTR value) {
            return principal.value()->put_Id(value);
        },
        "write the Task Scheduler principal ID");
    if (!written) {
        return written;
    }
    written = writeTaskText(
        expected.displayName,
        [&principal](BSTR value) {
            return principal.value()->put_DisplayName(value);
        },
        "write the Task Scheduler principal display name");
    if (!written) {
        return written;
    }
    written = writeTaskText(
        expected.userIdentity,
        [&principal](BSTR value) {
            return principal.value()->put_UserId(value);
        },
        "write the Task Scheduler principal user ID");
    if (!written) {
        return written;
    }
    if (!expected.groupIdentity.empty()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A Task Scheduler principal cannot set both user and group IDs."));
    }

    if (expected.logonType !=
        Manager::ManagerStartupTaskLogonType::CurrentInteractiveUser) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler principal logon type is unsupported."));
    }
    auto exact = exactComResult(
        principal.value()->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN),
        "write the Task Scheduler principal logon type");
    if (!exact) {
        return exact;
    }
    if (expected.runLevel !=
        Manager::ManagerStartupTaskRunLevel::LeastPrivilege) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler principal run level is unsupported."));
    }
    exact = exactComResult(
        principal.value()->put_RunLevel(TASK_RUNLEVEL_LUA),
        "write the Task Scheduler principal run level");
    if (!exact) {
        return exact;
    }

    auto principal2 = requiredQuery<IPrincipal2>(
        principal.value().get(),
        "open Task Scheduler principal version 2 for writing");
    if (!principal2) {
        return Domain::Result<void>::failure(
            std::move(principal2).error());
    }
    if (expected.processTokenSidType !=
        Manager::ManagerStartupTaskProcessTokenSidType::Default) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler process-token SID type is unsupported."));
    }
    exact = exactComResult(
        principal2.value()->put_ProcessTokenSidType(
            TASK_PROCESSTOKENSID_DEFAULT),
        "write the Task Scheduler process-token SID type");
    if (!exact) {
        return exact;
    }
    if (expected.requiredPrivileges.size() >
        Manager::ManagerStartupTaskPolicy::MaximumObservedPrivilegeCount) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "The canonical Task Scheduler privilege count exceeds its bound."));
    }
    for (const auto& privilege : expected.requiredPrivileges) {
        auto checkpoint = contextCheckpoint(
            context, "write a Task Scheduler required privilege");
        if (!checkpoint) {
            return checkpoint;
        }
        auto native = taskTextBstr(
            privilege, "A Task Scheduler required privilege");
        if (!native) {
            return Domain::Result<void>::failure(
                std::move(native).error());
        }
        exact = exactComResult(
            principal2.value()->AddRequiredPrivilege(native.value().get()),
            "write a Task Scheduler required privilege");
        if (!exact) {
            return exact;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> configureRepetition(
    ITrigger* trigger,
    const std::optional<Manager::ManagerStartupTaskRepetition>& expected)
{
    if (!expected.has_value()) {
        return Domain::Result<void>::success();
    }

    auto repetition = requiredInterface<IRepetitionPattern>(
        [trigger](IRepetitionPattern** value) {
            return trigger->get_Repetition(value);
        },
        "open the Task Scheduler repetition for writing");
    if (!repetition) {
        return Domain::Result<void>::failure(
            std::move(repetition).error());
    }

    auto written = writeDuration(
        expected->interval,
        [&repetition](BSTR value) {
            return repetition.value()->put_Interval(value);
        },
        "write the Task Scheduler repetition interval");
    if (!written) {
        return written;
    }
    written = writeDuration(
        expected->duration,
        [&repetition](BSTR value) {
            return repetition.value()->put_Duration(value);
        },
        "write the Task Scheduler repetition duration");
    if (!written) {
        return written;
    }
    return exactComResult(
        repetition.value()->put_StopAtDurationEnd(
            nativeBoolean(expected->stopAtDurationEnd)),
        "write the Task Scheduler repetition stop policy");
}

[[nodiscard]] Domain::Result<void> configureTrigger(
    ITrigger* trigger,
    const Manager::ManagerStartupTaskTrigger& expected)
{
    auto written = writeTaskText(
        expected.id,
        [trigger](BSTR value) { return trigger->put_Id(value); },
        "write the Task Scheduler trigger ID");
    if (!written) {
        return written;
    }
    written = configureRepetition(trigger, expected.repetition);
    if (!written) {
        return written;
    }
    written = writeDuration(
        expected.executionTimeLimit,
        [trigger](BSTR value) {
            return trigger->put_ExecutionTimeLimit(value);
        },
        "write the Task Scheduler trigger execution limit");
    if (!written) {
        return written;
    }
    written = writeOptionalTaskText(
        expected.startBoundary,
        [trigger](BSTR value) {
            return trigger->put_StartBoundary(value);
        },
        "write the Task Scheduler trigger start boundary");
    if (!written) {
        return written;
    }
    written = writeOptionalTaskText(
        expected.endBoundary,
        [trigger](BSTR value) {
            return trigger->put_EndBoundary(value);
        },
        "write the Task Scheduler trigger end boundary");
    if (!written) {
        return written;
    }
    auto exact = exactComResult(
        trigger->put_Enabled(nativeBoolean(expected.enabled)),
        "write the Task Scheduler trigger enabled state");
    if (!exact) {
        return exact;
    }

    auto logon = requiredQuery<ILogonTrigger>(
        trigger, "open the Task Scheduler logon trigger for writing");
    if (!logon) {
        return Domain::Result<void>::failure(std::move(logon).error());
    }
    written = writeTaskText(
        expected.userIdentity,
        [&logon](BSTR value) {
            return logon.value()->put_UserId(value);
        },
        "write the Task Scheduler logon-trigger user ID");
    if (!written) {
        return written;
    }
    return writeDuration(
        expected.delay,
        [&logon](BSTR value) {
            return logon.value()->put_Delay(value);
        },
        "write the Task Scheduler logon-trigger delay");
}

[[nodiscard]] Domain::Result<void> configureTriggers(
    ITaskDefinition* definition,
    const std::vector<Manager::ManagerStartupTaskTrigger>& expected,
    const Domain::OperationContext& context)
{
    if (expected.size() >
        Manager::ManagerStartupTaskPolicy::MaximumObservedTriggerCount) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "The canonical Task Scheduler trigger count exceeds its bound."));
    }
    auto collection = requiredInterface<ITriggerCollection>(
        [definition](ITriggerCollection** value) {
            return definition->get_Triggers(value);
        },
        "open the Task Scheduler trigger collection for writing");
    if (!collection) {
        return Domain::Result<void>::failure(
            std::move(collection).error());
    }
    auto exact = exactComResult(
        collection.value()->Clear(),
        "clear the Task Scheduler trigger collection");
    if (!exact) {
        return exact;
    }
    for (const auto& triggerDefinition : expected) {
        auto checkpoint = contextCheckpoint(
            context, "write a Task Scheduler trigger");
        if (!checkpoint) {
            return checkpoint;
        }
        if (triggerDefinition.kind !=
            Manager::ManagerStartupTaskTriggerKind::UserLogon) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The canonical Task Scheduler trigger kind is unsupported."));
        }
        auto trigger = requiredInterface<ITrigger>(
            [&collection](ITrigger** value) {
                return collection.value()->Create(
                    TASK_TRIGGER_LOGON, value);
            },
            "create the Task Scheduler logon trigger");
        if (!trigger) {
            return Domain::Result<void>::failure(
                std::move(trigger).error());
        }
        auto configured = configureTrigger(
            trigger.value().get(), triggerDefinition);
        if (!configured) {
            return configured;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> configureAction(
    IAction* action,
    const Manager::ManagerStartupTaskAction& expected)
{
    auto written = writeTaskText(
        expected.id,
        [action](BSTR value) { return action->put_Id(value); },
        "write the Task Scheduler action ID");
    if (!written) {
        return written;
    }

    auto executable = requiredQuery<IExecAction>(
        action, "open the Task Scheduler executable action for writing");
    if (!executable) {
        return Domain::Result<void>::failure(
            std::move(executable).error());
    }
    const std::string_view path = expected.executable.has_value()
        ? std::string_view{expected.executable->value()}
        : std::string_view{};
    written = writeTaskText(
        path,
        [&executable](BSTR value) {
            return executable.value()->put_Path(value);
        },
        "write the Task Scheduler executable path");
    if (!written) {
        return written;
    }
    written = writeTaskText(
        expected.arguments,
        [&executable](BSTR value) {
            return executable.value()->put_Arguments(value);
        },
        "write the Task Scheduler executable arguments");
    if (!written) {
        return written;
    }
    const std::string_view workingDirectory =
        expected.workingDirectory.has_value()
        ? std::string_view{expected.workingDirectory->value()}
        : std::string_view{};
    written = writeTaskText(
        workingDirectory,
        [&executable](BSTR value) {
            return executable.value()->put_WorkingDirectory(value);
        },
        "write the Task Scheduler executable working directory");
    if (!written) {
        return written;
    }

    auto executable2 = requiredQuery<IExecAction2>(
        executable.value().get(),
        "open Task Scheduler executable action version 2 for writing");
    if (!executable2) {
        return Domain::Result<void>::failure(
            std::move(executable2).error());
    }
    return exactComResult(
        executable2.value()->put_HideAppWindow(
            nativeBoolean(expected.hideAppWindow)),
        "write the Task Scheduler executable hidden-window policy");
}

[[nodiscard]] Domain::Result<void> configureActions(
    ITaskDefinition* definition,
    const std::string_view actionContext,
    const std::vector<Manager::ManagerStartupTaskAction>& expected,
    const Domain::OperationContext& context)
{
    if (expected.size() >
        Manager::ManagerStartupTaskPolicy::MaximumObservedActionCount) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "The canonical Task Scheduler action count exceeds its bound."));
    }
    auto collection = requiredInterface<IActionCollection>(
        [definition](IActionCollection** value) {
            return definition->get_Actions(value);
        },
        "open the Task Scheduler action collection for writing");
    if (!collection) {
        return Domain::Result<void>::failure(
            std::move(collection).error());
    }
    auto exact = exactComResult(
        collection.value()->Clear(),
        "clear the Task Scheduler action collection");
    if (!exact) {
        return exact;
    }
    auto written = writeTaskText(
        actionContext,
        [&collection](BSTR value) {
            return collection.value()->put_Context(value);
        },
        "write the Task Scheduler action context");
    if (!written) {
        return written;
    }

    for (const auto& actionDefinition : expected) {
        auto checkpoint = contextCheckpoint(
            context, "write a Task Scheduler action");
        if (!checkpoint) {
            return checkpoint;
        }
        if (actionDefinition.kind !=
            Manager::ManagerStartupTaskActionKind::Execute) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The canonical Task Scheduler action kind is unsupported."));
        }
        auto action = requiredInterface<IAction>(
            [&collection](IAction** value) {
                return collection.value()->Create(TASK_ACTION_EXEC, value);
            },
            "create the Task Scheduler executable action");
        if (!action) {
            return Domain::Result<void>::failure(
                std::move(action).error());
        }
        auto configured = configureAction(
            action.value().get(), actionDefinition);
        if (!configured) {
            return configured;
        }
    }
    return Domain::Result<void>::success();
}

template <typename Setter>
[[nodiscard]] Domain::Result<void> writeBoolean(
    const bool value,
    Setter&& setter,
    const std::string_view action)
{
    return exactComResult(setter(nativeBoolean(value)), action);
}

[[nodiscard]] Domain::Result<void> configureSettings(
    ITaskDefinition* definition,
    const Manager::ManagerStartupTaskSettings& expected,
    const bool enabled)
{
    auto settings = requiredInterface<ITaskSettings>(
        [definition](ITaskSettings** value) {
            return definition->get_Settings(value);
        },
        "open the Task Scheduler settings for writing");
    if (!settings) {
        return Domain::Result<void>::failure(
            std::move(settings).error());
    }

    auto written = writeBoolean(
        expected.allowDemandStart,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_AllowDemandStart(value);
        },
        "write the Task Scheduler demand-start policy");
    if (!written) {
        return written;
    }
    written = writeDuration(
        expected.restartInterval,
        [&settings](BSTR value) {
            return settings.value()->put_RestartInterval(value);
        },
        "write the Task Scheduler restart interval");
    if (!written) {
        return written;
    }
    auto exact = exactComResult(
        settings.value()->put_RestartCount(
            static_cast<int>(expected.restartCount)),
        "write the Task Scheduler restart count");
    if (!exact) {
        return exact;
    }
    if (expected.multipleInstances !=
        Manager::ManagerStartupTaskMultipleInstances::IgnoreNew) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler multiple-instance policy is unsupported."));
    }
    exact = exactComResult(
        settings.value()->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW),
        "write the Task Scheduler multiple-instance policy");
    if (!exact) {
        return exact;
    }

    written = writeBoolean(
        expected.stopIfGoingOnBatteries,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_StopIfGoingOnBatteries(value);
        },
        "write the Task Scheduler stop-on-battery policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.disallowStartIfOnBatteries,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_DisallowStartIfOnBatteries(value);
        },
        "write the Task Scheduler battery-start policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.allowHardTerminate,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_AllowHardTerminate(value);
        },
        "write the Task Scheduler hard-termination policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.startWhenAvailable,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_StartWhenAvailable(value);
        },
        "write the Task Scheduler missed-start policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.runOnlyIfNetworkAvailable,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_RunOnlyIfNetworkAvailable(value);
        },
        "write the Task Scheduler network gate");
    if (!written) {
        return written;
    }
    written = writeDuration(
        expected.executionTimeLimit,
        [&settings](BSTR value) {
            return settings.value()->put_ExecutionTimeLimit(value);
        },
        "write the Task Scheduler execution limit");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        enabled,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_Enabled(value);
        },
        "write the task-definition enabled state");
    if (!written) {
        return written;
    }
    const auto* deleteExpiredTaskAfter =
        expected.deleteExpiredTaskAfter.fixedSeconds();
    if (deleteExpiredTaskAfter == nullptr ||
        *deleteExpiredTaskAfter != std::chrono::seconds::zero()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler expiration delay must be zero when no trigger end boundary is present."));
    }
    auto absentExpiration = boundedBstr(
        L"", 0U, "The absent Task Scheduler expiration delay");
    if (!absentExpiration) {
        return Domain::Result<void>::failure(
            std::move(absentExpiration).error());
    }
    exact = exactComResult(
        settings.value()->put_DeleteExpiredTaskAfter(
            absentExpiration.value().get()),
        "clear the Task Scheduler expiration delay");
    if (!exact) {
        return exact;
    }
    exact = exactComResult(
        settings.value()->put_Priority(static_cast<int>(expected.priority)),
        "write the Task Scheduler priority");
    if (!exact) {
        return exact;
    }
    if (expected.compatibility !=
        Manager::ManagerStartupTaskCompatibility::Windows10OrLater) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Task Scheduler compatibility level is unsupported."));
    }
    exact = exactComResult(
        settings.value()->put_Compatibility(TASK_COMPATIBILITY_V2_4),
        "write the Task Scheduler compatibility level");
    if (!exact) {
        return exact;
    }
    written = writeBoolean(
        expected.hidden,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_Hidden(value);
        },
        "write the Task Scheduler hidden-task policy");
    if (!written) {
        return written;
    }

    auto idle = requiredInterface<IIdleSettings>(
        [&settings](IIdleSettings** value) {
            return settings.value()->get_IdleSettings(value);
        },
        "open the Task Scheduler idle settings for writing");
    if (!idle) {
        return Domain::Result<void>::failure(std::move(idle).error());
    }
    written = writeDuration(
        expected.idleDuration,
        [&idle](BSTR value) {
            return idle.value()->put_IdleDuration(value);
        },
        "write the Task Scheduler idle duration");
    if (!written) {
        return written;
    }
    written = writeDuration(
        expected.idleWaitTimeout,
        [&idle](BSTR value) {
            return idle.value()->put_WaitTimeout(value);
        },
        "write the Task Scheduler idle wait timeout");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.stopOnIdleEnd,
        [&idle](VARIANT_BOOL value) {
            return idle.value()->put_StopOnIdleEnd(value);
        },
        "write the Task Scheduler stop-on-idle-end policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.restartOnIdle,
        [&idle](VARIANT_BOOL value) {
            return idle.value()->put_RestartOnIdle(value);
        },
        "write the Task Scheduler restart-on-idle policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.runOnlyIfIdle,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_RunOnlyIfIdle(value);
        },
        "write the Task Scheduler idle gate");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.wakeToRun,
        [&settings](VARIANT_BOOL value) {
            return settings.value()->put_WakeToRun(value);
        },
        "write the Task Scheduler wake-to-run policy");
    if (!written) {
        return written;
    }

    auto network = requiredInterface<INetworkSettings>(
        [&settings](INetworkSettings** value) {
            return settings.value()->get_NetworkSettings(value);
        },
        "open the Task Scheduler network settings for writing");
    if (!network) {
        return Domain::Result<void>::failure(
            std::move(network).error());
    }
    written = writeTaskText(
        expected.networkProfileName,
        [&network](BSTR value) {
            return network.value()->put_Name(value);
        },
        "write the Task Scheduler network profile name");
    if (!written) {
        return written;
    }
    written = writeTaskText(
        expected.networkProfileId,
        [&network](BSTR value) {
            return network.value()->put_Id(value);
        },
        "write the Task Scheduler network profile ID");
    if (!written) {
        return written;
    }

    auto settings2 = requiredQuery<ITaskSettings2>(
        settings.value().get(),
        "open Task Scheduler settings version 2 for writing");
    if (!settings2) {
        return Domain::Result<void>::failure(
            std::move(settings2).error());
    }
    written = writeBoolean(
        expected.disallowStartOnRemoteAppSession,
        [&settings2](VARIANT_BOOL value) {
            return settings2.value()->put_DisallowStartOnRemoteAppSession(value);
        },
        "write the Task Scheduler RemoteApp-session policy");
    if (!written) {
        return written;
    }
    written = writeBoolean(
        expected.useUnifiedSchedulingEngine,
        [&settings2](VARIANT_BOOL value) {
            return settings2.value()->put_UseUnifiedSchedulingEngine(value);
        },
        "write the Task Scheduler unified-engine policy");
    if (!written) {
        return written;
    }

    auto settings3 = requiredQuery<ITaskSettings3>(
        settings.value().get(),
        "open Task Scheduler settings version 3 for writing");
    if (!settings3) {
        return Domain::Result<void>::failure(
            std::move(settings3).error());
    }
    if (expected.maintenanceSettingsPresent) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The canonical Manager startup task cannot request maintenance settings."));
    }
    UniqueComInterface<IMaintenanceSettings> maintenance;
    exact = exactComResult(
        settings3.value()->get_MaintenanceSettings(maintenance.put()),
        "verify absent Task Scheduler maintenance settings");
    if (!exact) {
        return exact;
    }
    if (maintenance) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A new Task Scheduler definition unexpectedly contains maintenance settings."));
    }
    return writeBoolean(
        expected.volatileTask,
        [&settings3](VARIANT_BOOL value) {
            return settings3.value()->put_Volatile(value);
        },
        "write the Task Scheduler volatile-task policy");
}

[[nodiscard]] Domain::Result<void> configureDefinition(
    ITaskDefinition* definition,
    const Manager::ManagerStartupTaskDefinition& expected,
    const bool enabled,
    const Domain::OperationContext& context)
{
    auto configured = configureOwnership(definition, expected.ownership);
    if (!configured) {
        return configured;
    }
    configured = configurePrincipal(
        definition, expected.principal, context);
    if (!configured) {
        return configured;
    }
    auto checkpoint = contextCheckpoint(
        context, "write the Manager startup triggers");
    if (!checkpoint) {
        return checkpoint;
    }
    configured = configureTriggers(definition, expected.triggers, context);
    if (!configured) {
        return configured;
    }
    checkpoint = contextCheckpoint(
        context, "write the Manager startup actions");
    if (!checkpoint) {
        return checkpoint;
    }
    configured = configureActions(
        definition, expected.actionContext, expected.actions, context);
    if (!configured) {
        return configured;
    }
    checkpoint = contextCheckpoint(
        context, "write the Manager startup settings");
    if (!checkpoint) {
        return checkpoint;
    }
    configured = configureSettings(definition, expected.settings, enabled);
    if (!configured) {
        return configured;
    }
    return contextCheckpoint(
        context, "finish writing the Manager startup definition");
}

[[nodiscard]] Domain::Result<void> setVariantText(
    UniqueVariant& variant,
    const std::string_view value,
    const std::string_view label)
{
    if (variant.get().vt != VT_EMPTY) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{label} + " variant was not empty."));
    }
    auto text = taskTextBstr(value, label);
    if (!text) {
        return Domain::Result<void>::failure(std::move(text).error());
    }
    variant.get().vt = VT_BSTR;
    variant.get().bstrVal = text.value().detach();
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<Domain::ManagerStartupStatus> classifyExisting(
    IRegisteredTask* task,
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context)
{
    auto observation = projectRegisteredTask(task, registration, context);
    if (!observation) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(observation).error());
    }
    return Manager::ManagerStartupTaskPolicy::classify(
        registration.definition, observation.value());
}

[[nodiscard]] Domain::Result<void> mutationConflict(std::string message)
{
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::Conflict, std::move(message)));
}

[[nodiscard]] Domain::Result<void> ownershipConflict()
{
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::OwnershipConflict,
        "The Manager startup task is no longer owned by Forge Conductor."));
}

struct VerifiedMutationTask final {
    SchedulerSession session;
    UniqueComInterface<IRegisteredTask> task;
    Domain::ManagerStartupStatus status;
};

[[nodiscard]] Domain::Result<VerifiedMutationTask> verifyMutationTask(
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context)
{
    auto session = connectScheduler(context);
    if (!session) {
        return Domain::Result<VerifiedMutationTask>::failure(
            std::move(session).error());
    }
    auto lookup = lookupRegisteredTask(
        session.value().root.get(), registration);
    if (!lookup) {
        return Domain::Result<VerifiedMutationTask>::failure(
            std::move(lookup).error());
    }
    if (lookup.value().missing) {
        return failure<VerifiedMutationTask>(
            Domain::ErrorCodes::Conflict,
            "The Manager startup task disappeared before mutation.");
    }
    auto status = classifyExisting(
        lookup.value().task.get(), registration, context);
    if (!status) {
        return Domain::Result<VerifiedMutationTask>::failure(
            std::move(status).error());
    }
    if (status.value().state ==
        Domain::ManagerStartupState::ForeignConflict) {
        return failure<VerifiedMutationTask>(
            Domain::ErrorCodes::OwnershipConflict,
            "The Manager startup task ownership changed before mutation.");
    }
    if (status.value().state == Domain::ManagerStartupState::Missing) {
        return failure<VerifiedMutationTask>(
            Domain::ErrorCodes::Conflict,
            "The Manager startup task became missing before mutation.");
    }
    return Domain::Result<VerifiedMutationTask>::success(
        VerifiedMutationTask{
            std::move(session).value(),
            std::move(lookup).value().task,
            std::move(status).value()});
}

[[nodiscard]] Domain::Result<void> registerDefinition(
    SchedulerSession& session,
    const ManagerStartupResolvedRegistration& registration,
    const LONG creationFlags,
    const bool enabled,
    const Domain::OperationContext& context)
{
    auto definition = requiredInterface<ITaskDefinition>(
        [&session](ITaskDefinition** value) {
            return session.service->NewTask(0U, value);
        },
        "create a new Task Scheduler definition");
    if (!definition) {
        return Domain::Result<void>::failure(
            std::move(definition).error());
    }
    auto configured = configureDefinition(
        definition.value().get(), registration.definition, enabled, context);
    if (!configured) {
        return configured;
    }
    auto checkpoint = contextCheckpoint(
        context, "register the canonical Manager startup task");
    if (!checkpoint) {
        return checkpoint;
    }

    auto taskPath = taskPathBstr(registration);
    if (!taskPath) {
        return Domain::Result<void>::failure(
            std::move(taskPath).error());
    }
    UniqueVariant user;
    auto userSet = setVariantText(
        user,
        registration.definition.principal.userIdentity,
        "The Task Scheduler registration user ID");
    if (!userSet) {
        return userSet;
    }
    UniqueVariant password;
    UniqueVariant securityDescriptor;
    UniqueComInterface<IRegisteredTask> registered;
    const LONG flags = creationFlags |
        (enabled ? 0L : static_cast<LONG>(TASK_DISABLE));
    const HRESULT result = session.root->RegisterTaskDefinition(
        taskPath.value().get(),
        definition.value().get(),
        flags,
        user.get(),
        password.get(),
        TASK_LOGON_INTERACTIVE_TOKEN,
        securityDescriptor.get(),
        registered.put());
    if (result != S_OK) {
        const bool collision =
            result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
            result == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) ||
            (creationFlags == TASK_UPDATE && isMissingTaskResult(result));
        return Domain::Result<void>::failure(makeHResultError(
            "register the canonical Manager startup task",
            result,
            collision
                ? Domain::ErrorCodes::Conflict
                : Domain::ErrorCodes::InternalFailure));
    }
    if (!registered) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Task Scheduler returned a null task after registration."));
    }
    return contextCheckpoint(
        context, "finish registering the canonical Manager startup task");
}

[[nodiscard]] bool exactOwnedState(
    const Domain::ManagerStartupStatus& status) noexcept
{
    return status.state == Domain::ManagerStartupState::Ready ||
        status.state == Domain::ManagerStartupState::Disabled;
}

} // namespace

Domain::Result<ManagerStartupResolvedRegistration>
WindowsTaskSchedulerStartupPlatform::resolve(
    const Domain::ManagerStartupDefinition& expected,
    const std::string_view purposeSuffix,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto checkpoint = contextCheckpoint(
            context, "resolve the Manager startup registration");
        if (!checkpoint) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(checkpoint).error());
        }
        auto identity = WindowsCurrentUserIdentity::load();
        if (!identity) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(identity).error());
        }
        checkpoint = contextCheckpoint(
            context, "build the Manager startup registration");
        if (!checkpoint) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(checkpoint).error());
        }
        auto registration = ManagerStartupDefinitionBuilder::build(
            expected, identity.value(), purposeSuffix);
        if (!registration) {
            return registration;
        }
        checkpoint = contextCheckpoint(
            context, "finish resolving the Manager startup registration");
        if (!checkpoint) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(checkpoint).error());
        }
        return registration;
    } catch (...) {
        return failure<ManagerStartupResolvedRegistration>(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not resolve the registration.");
    }
}

Domain::Result<Manager::ManagerStartupTaskObservation>
WindowsTaskSchedulerStartupPlatform::inspect(
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto session = connectScheduler(context);
        if (!session) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                std::move(session).error());
        }
        auto lookup = lookupRegisteredTask(
            session.value().root.get(), registration);
        if (!lookup) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                std::move(lookup).error());
        }
        if (lookup.value().missing) {
            auto checkpoint = contextCheckpoint(
                context, "finish inspecting the missing Manager startup task");
            if (!checkpoint) {
                return Domain::Result<
                    Manager::ManagerStartupTaskObservation>::failure(
                    std::move(checkpoint).error());
            }
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::success({});
        }
        return projectRegisteredTask(
            lookup.value().task.get(), registration, context);
    } catch (...) {
        return failure<Manager::ManagerStartupTaskObservation>(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not inspect the registration.");
    }
}

Domain::Result<void> WindowsTaskSchedulerStartupPlatform::registerCanonical(
    const ManagerStartupResolvedRegistration& registration,
    const ManagerStartupRegistrationMutation mutation,
    const bool enabled,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto session = connectScheduler(context);
        if (!session) {
            return Domain::Result<void>::failure(
                std::move(session).error());
        }
        auto lookup = lookupRegisteredTask(
            session.value().root.get(), registration);
        if (!lookup) {
            return Domain::Result<void>::failure(
                std::move(lookup).error());
        }

        LONG creationFlags{};
        switch (mutation) {
        case ManagerStartupRegistrationMutation::CreateMissing:
            if (!lookup.value().missing) {
                return mutationConflict(
                    "A Manager startup task appeared before creation.");
            }
            creationFlags = TASK_CREATE;
            break;

        case ManagerStartupRegistrationMutation::ReplaceOwned:
            if (lookup.value().missing) {
                return mutationConflict(
                    "The Manager startup task disappeared before replacement.");
            }
            {
                auto status = classifyExisting(
                    lookup.value().task.get(), registration, context);
                if (!status) {
                    return Domain::Result<void>::failure(
                        std::move(status).error());
                }
                if (status.value().state ==
                    Domain::ManagerStartupState::ForeignConflict) {
                    return ownershipConflict();
                }
                if (status.value().state ==
                    Domain::ManagerStartupState::Missing) {
                    return mutationConflict(
                        "The Manager startup task became missing before replacement.");
                }
            }
            creationFlags = TASK_UPDATE;
            break;

        default:
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Task Scheduler registration mutation is not recognized."));
        }

        return registerDefinition(
            session.value(),
            registration,
            creationFlags,
            enabled,
            context);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not register the task."));
    }
}

Domain::Result<void> WindowsTaskSchedulerStartupPlatform::setEnabled(
    const ManagerStartupResolvedRegistration& registration,
    const bool enabled,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto verified = verifyMutationTask(registration, context);
        if (!verified) {
            return Domain::Result<void>::failure(
                std::move(verified).error());
        }
        const auto state = verified.value().status.state;
        const bool allowed = enabled
            ? exactOwnedState(verified.value().status)
            : exactOwnedState(verified.value().status) ||
                state == Domain::ManagerStartupState::Drifted;
        if (!allowed) {
            return mutationConflict(
                "The Manager startup task changed to a state that cannot "
                "accept the requested enablement mutation.");
        }
        auto checkpoint = contextCheckpoint(
            context, "change the Manager startup task enabled state");
        if (!checkpoint) {
            return checkpoint;
        }
        auto exact = exactComResult(
            verified.value().task->put_Enabled(nativeBoolean(enabled)),
            "change the Manager startup task enabled state");
        if (!exact) {
            return exact;
        }
        return contextCheckpoint(
            context, "finish changing the Manager startup task enabled state");
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not change task enablement."));
    }
}

Domain::Result<void> WindowsTaskSchedulerStartupPlatform::startNow(
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto verified = verifyMutationTask(registration, context);
        if (!verified) {
            return Domain::Result<void>::failure(
                std::move(verified).error());
        }
        if (verified.value().status.state !=
            Domain::ManagerStartupState::Ready) {
            return mutationConflict(
                "The Manager startup task changed before the on-demand start.");
        }
        auto checkpoint = contextCheckpoint(
            context, "start the Manager startup task on demand");
        if (!checkpoint) {
            return checkpoint;
        }
        UniqueVariant parameters;
        UniqueComInterface<IRunningTask> running;
        auto exact = exactComResult(
            verified.value().task->Run(parameters.get(), running.put()),
            "start the Manager startup task on demand");
        if (!exact) {
            return exact;
        }
        if (!running) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Task Scheduler returned a null running-task interface."));
        }
        return contextCheckpoint(
            context, "finish starting the Manager startup task on demand");
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not start the task."));
    }
}

Domain::Result<void> WindowsTaskSchedulerStartupPlatform::remove(
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto verified = verifyMutationTask(registration, context);
        if (!verified) {
            return Domain::Result<void>::failure(
                std::move(verified).error());
        }
        const auto state = verified.value().status.state;
        if (!exactOwnedState(verified.value().status) &&
            state != Domain::ManagerStartupState::Drifted) {
            return mutationConflict(
                "The Manager startup task changed before removal.");
        }
        auto checkpoint = contextCheckpoint(
            context, "remove the Manager startup task");
        if (!checkpoint) {
            return checkpoint;
        }
        auto taskPath = taskPathBstr(registration);
        if (!taskPath) {
            return Domain::Result<void>::failure(
                std::move(taskPath).error());
        }
        const HRESULT result = verified.value().session.root->DeleteTask(
            taskPath.value().get(), 0L);
        if (isMissingTaskResult(result)) {
            return mutationConflict(
                "The Manager startup task disappeared during removal.");
        }
        auto exact = exactComResult(
            result, "remove the Manager startup task");
        if (!exact) {
            return exact;
        }
        return contextCheckpoint(
            context, "finish removing the Manager startup task");
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler startup platform could not remove the task."));
    }
}

std::shared_ptr<IWindowsTaskSchedulerStartupPlatform>
createWindowsTaskSchedulerStartupPlatform()
{
    return std::make_shared<WindowsTaskSchedulerStartupPlatform>();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
