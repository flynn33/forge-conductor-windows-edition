#include "ForgeConductor/Infrastructure/Windows/WindowsManagerStartupService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <taskschd.h>

#include "Infrastructure/Windows/Detail/UniqueBstr.h"
#include "Infrastructure/Windows/Detail/UniqueComInterface.h"
#include "Infrastructure/Windows/Detail/UniqueVariant.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using namespace std::chrono_literals;

constexpr std::string_view PurposeSuffix = "p16-lifecycle-integration";
constexpr std::wstring_view SentinelFileName =
    L"manager-startup-lifecycle.sentinel";
constexpr std::string_view SentinelPayload =
    "forge-conductor-manager-startup-lifecycle-v1\r\n";
constexpr auto OperationTimeout = 15s;
constexpr auto SentinelTimeout = 10s;
constexpr auto StopTimeout = 5s;
constexpr auto PollInterval = 50ms;
constexpr std::size_t MaximumPathCharacters = 32'767U;

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

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

class UniqueHandle final {
public:
    explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    void reset() noexcept
    {
        if (valid()) {
            static_cast<void>(::CloseHandle(value_));
        }
        value_ = INVALID_HANDLE_VALUE;
    }

    HANDLE value_{INVALID_HANDLE_VALUE};
};

class OperationSequence final {
public:
    [[nodiscard]] Domain::OperationContext next()
    {
        std::string operation{"70000000-0000-4000-8000-000000000000"};
        auto remaining = ++counter_;
        constexpr char Digits[] = "0123456789abcdef";
        for (std::size_t index{}; index < 8U; ++index) {
            operation[operation.size() - 1U - index] =
                Digits[remaining & 0xFU];
            remaining >>= 4U;
        }
        return Domain::OperationContext{
            take(Domain::OperationId::parse(operation)),
            std::chrono::steady_clock::now() + OperationTimeout,
            std::stop_token{},
            take(Domain::CorrelationId::parse(
                "p16-manager-startup-lifecycle"))};
    }

private:
    std::uint32_t counter_{};
};

[[nodiscard]] bool isOwnedState(
    const Domain::ManagerStartupState state) noexcept
{
    return state == Domain::ManagerStartupState::Ready ||
        state == Domain::ManagerStartupState::Disabled ||
        state == Domain::ManagerStartupState::Drifted;
}

[[nodiscard]] std::wstring absolutePath(const std::wstring_view input)
{
    require(!input.empty() && input.size() <= MaximumPathCharacters,
            "the fixture path is empty or exceeds the native path bound");

    std::vector<wchar_t> buffer(MaximumPathCharacters + 1U, L'\0');
    const DWORD length = ::GetFullPathNameW(
        std::wstring{input}.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    require(length != 0U && length < buffer.size(),
            "GetFullPathNameW could not resolve the fixture executable");

    std::wstring result{buffer.data(), static_cast<std::size_t>(length)};
    std::replace(result.begin(), result.end(), L'/', L'\\');
    const DWORD attributes = ::GetFileAttributesW(result.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U,
            "the fixture executable does not name an existing file");
    return result;
}

[[nodiscard]] std::wstring makeTemporaryHome()
{
    std::array<wchar_t, MaximumPathCharacters + 1U> buffer{};
    const DWORD length = ::GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    require(length != 0U && length < buffer.size(),
            "GetTempPathW could not resolve the lifecycle test root");

    std::wstring path{buffer.data(), static_cast<std::size_t>(length)};
    require(path.size() >= 3U && path[1U] == L':' && path[2U] == L'\\',
            "the lifecycle test requires a local-drive temporary root");
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += L"ForgeConductor.Manager.StartupLifecycle.";
    path += std::to_wstring(::GetCurrentProcessId());
    path.push_back(L'.');
    path += std::to_wstring(::GetTickCount64());
    require(path.size() <= MaximumPathCharacters,
            "the lifecycle test home exceeds the native path bound");
    require(::CreateDirectoryW(path.c_str(), nullptr) != FALSE,
            "CreateDirectoryW could not create the lifecycle test home");
    return path;
}

class TemporaryHome final {
public:
    TemporaryHome()
        : path_{makeTemporaryHome()},
          sentinel_{path_ + L"\\" + std::wstring{SentinelFileName}}
    {
    }

    ~TemporaryHome()
    {
        static_cast<void>(cleanup());
    }

    TemporaryHome(const TemporaryHome&) = delete;
    TemporaryHome& operator=(const TemporaryHome&) = delete;

    [[nodiscard]] const std::wstring& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] const std::wstring& sentinel() const noexcept
    {
        return sentinel_;
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        if (cleaned_) {
            return true;
        }
        if (!::DeleteFileW(sentinel_.c_str())) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) {
                return false;
            }
        }
        if (!::RemoveDirectoryW(path_.c_str())) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND) {
                return false;
            }
        }
        cleaned_ = true;
        return true;
    }

private:
    std::wstring path_;
    std::wstring sentinel_;
    bool cleaned_{};
};

class ScopedComApartment final {
public:
    ScopedComApartment() noexcept
        : result_{::CoInitializeEx(nullptr, COINIT_MULTITHREADED)},
          uninitialize_{result_ == S_OK || result_ == S_FALSE}
    {
    }

    ~ScopedComApartment()
    {
        if (uninitialize_) {
            ::CoUninitialize();
        }
    }

    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    [[nodiscard]] bool usable() const noexcept
    {
        return uninitialize_ || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{};
    bool uninitialize_{};
};

enum class DirectCleanupResult {
    Missing,
    Removed,
    Refused,
    Failed
};

[[nodiscard]] DirectCleanupResult cleanupOwnedTaskDirectly(
    const Detail::ManagerStartupResolvedRegistration& registration) noexcept
{
    try {
        ScopedComApartment apartment;
        if (!apartment.usable()) {
            return DirectCleanupResult::Failed;
        }

        Detail::UniqueComInterface<ITaskService> scheduler;
        if (::CoCreateInstance(
                CLSID_TaskScheduler,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_ITaskService,
                reinterpret_cast<void**>(scheduler.put())) != S_OK ||
            !scheduler) {
            return DirectCleanupResult::Failed;
        }
        Detail::UniqueVariant server;
        Detail::UniqueVariant user;
        Detail::UniqueVariant domain;
        Detail::UniqueVariant password;
        if (scheduler->Connect(
                server.get(), user.get(), domain.get(), password.get()) !=
            S_OK) {
            return DirectCleanupResult::Failed;
        }

        auto rootPath = Detail::UniqueBstr::copy(L"\\");
        Detail::UniqueComInterface<ITaskFolder> root;
        if (!rootPath || scheduler->GetFolder(rootPath.get(), root.put()) != S_OK ||
            !root) {
            return DirectCleanupResult::Failed;
        }
        auto taskPath = Detail::UniqueBstr::copy(registration.taskPath);
        if (!taskPath) {
            return DirectCleanupResult::Failed;
        }

        Detail::UniqueComInterface<IRegisteredTask> task;
        const HRESULT lookup = root->GetTask(taskPath.get(), task.put());
        if (lookup == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return DirectCleanupResult::Missing;
        }
        if (lookup != S_OK || !task) {
            return DirectCleanupResult::Failed;
        }

        Detail::UniqueBstr observedPath;
        if (task->get_Path(observedPath.put()) != S_OK ||
            observedPath.view() != registration.taskPath) {
            return DirectCleanupResult::Refused;
        }
        Detail::UniqueComInterface<ITaskDefinition> definition;
        if (task->get_Definition(definition.put()) != S_OK || !definition) {
            return DirectCleanupResult::Failed;
        }
        Detail::UniqueComInterface<IRegistrationInfo> registrationInfo;
        if (definition->get_RegistrationInfo(registrationInfo.put()) != S_OK ||
            !registrationInfo) {
            return DirectCleanupResult::Failed;
        }

        Detail::UniqueBstr source;
        Detail::UniqueBstr uri;
        if (registrationInfo->get_Source(source.put()) != S_OK ||
            registrationInfo->get_URI(uri.put()) != S_OK ||
            source.view().size() >
                ForgeConductor::Manager::ManagerStartupTaskPolicy::MaximumTextBytes ||
            uri.view().size() >
                ForgeConductor::Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units ||
            source.view().find(L'\0') != std::wstring_view::npos ||
            uri.view().find(L'\0') != std::wstring_view::npos) {
            return DirectCleanupResult::Failed;
        }
        auto sourceUtf8 = Detail::strictUtf16ToUtf8(source.view());
        auto uriUtf8 = Detail::strictUtf16ToUtf8(uri.view());
        if (!sourceUtf8 || !uriUtf8) {
            return DirectCleanupResult::Failed;
        }
        if (sourceUtf8.value() != registration.definition.ownership.source ||
            uriUtf8.value() != registration.definition.ownership.uri) {
            return DirectCleanupResult::Refused;
        }

        const HRESULT deleted = root->DeleteTask(taskPath.get(), 0L);
        if (deleted != S_OK &&
            deleted != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return DirectCleanupResult::Failed;
        }
        Detail::UniqueComInterface<IRegisteredTask> remaining;
        return root->GetTask(taskPath.get(), remaining.put()) ==
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            ? DirectCleanupResult::Removed
            : DirectCleanupResult::Failed;
    } catch (...) {
        return DirectCleanupResult::Failed;
    }
}

void driftOwnedTaskPriority(
    const Detail::ManagerStartupResolvedRegistration& registration)
{
    ScopedComApartment apartment;
    require(
        apartment.usable(),
        "COM could not initialize for the owned priority-drift mutation");

    Detail::UniqueComInterface<ITaskService> scheduler;
    require(
        ::CoCreateInstance(
            CLSID_TaskScheduler,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_ITaskService,
            reinterpret_cast<void**>(scheduler.put())) == S_OK &&
            scheduler,
        "Task Scheduler could not open for the owned priority-drift mutation");
    Detail::UniqueVariant server;
    Detail::UniqueVariant user;
    Detail::UniqueVariant domain;
    Detail::UniqueVariant password;
    require(
        scheduler->Connect(
            server.get(), user.get(), domain.get(), password.get()) == S_OK,
        "Task Scheduler could not connect for the owned priority-drift mutation");

    auto rootPath = Detail::UniqueBstr::copy(L"\\");
    Detail::UniqueComInterface<ITaskFolder> root;
    require(
        rootPath &&
            scheduler->GetFolder(rootPath.get(), root.put()) == S_OK && root,
        "Task Scheduler could not open its root for the owned priority-drift mutation");
    auto taskPath = Detail::UniqueBstr::copy(registration.taskPath);
    require(
        static_cast<bool>(taskPath),
        "the isolated task path could not be allocated");

    Detail::UniqueComInterface<IRegisteredTask> task;
    require(
        root->GetTask(taskPath.get(), task.put()) == S_OK && task,
        "the isolated task disappeared before the owned priority-drift mutation");
    Detail::UniqueBstr observedPath;
    require(
        task->get_Path(observedPath.put()) == S_OK &&
            observedPath.view() == registration.taskPath,
        "the registered path changed before the owned priority-drift mutation");

    Detail::UniqueComInterface<ITaskDefinition> definition;
    require(
        task->get_Definition(definition.put()) == S_OK && definition,
        "the isolated task definition could not be read before the owned priority-drift mutation");
    Detail::UniqueComInterface<IRegistrationInfo> registrationInfo;
    require(
        definition->get_RegistrationInfo(registrationInfo.put()) == S_OK &&
            registrationInfo,
        "the isolated ownership metadata could not be opened before the priority-drift mutation");
    Detail::UniqueBstr source;
    Detail::UniqueBstr uri;
    require(
        registrationInfo->get_Source(source.put()) == S_OK &&
            registrationInfo->get_URI(uri.put()) == S_OK &&
            source.view().size() <=
                ForgeConductor::Manager::ManagerStartupTaskPolicy::MaximumTextBytes &&
            uri.view().size() <=
                ForgeConductor::Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units &&
            source.view().find(L'\0') == std::wstring_view::npos &&
            uri.view().find(L'\0') == std::wstring_view::npos,
        "the isolated ownership metadata was invalid before the priority-drift mutation");
    const auto sourceUtf8 = take(Detail::strictUtf16ToUtf8(source.view()));
    const auto uriUtf8 = take(Detail::strictUtf16ToUtf8(uri.view()));
    require(
        sourceUtf8 == registration.definition.ownership.source &&
            uriUtf8 == registration.definition.ownership.uri,
        "the isolated task was not Forge-owned; refusing the priority-drift mutation");

    VARIANT_BOOL registeredEnabled{VARIANT_TRUE};
    require(
        task->get_Enabled(&registeredEnabled) == S_OK &&
            registeredEnabled == VARIANT_FALSE,
        "the isolated task was not disabled before the priority-drift mutation");
    Detail::UniqueComInterface<ITaskSettings> settings;
    require(
        definition->get_Settings(settings.put()) == S_OK && settings,
        "the isolated task settings could not be opened before the priority-drift mutation");
    VARIANT_BOOL definitionEnabled{VARIANT_TRUE};
    int priority{};
    require(
        settings->get_Enabled(&definitionEnabled) == S_OK &&
            definitionEnabled == VARIANT_FALSE &&
            settings->get_Priority(&priority) == S_OK &&
            priority ==
                static_cast<int>(
                    ForgeConductor::Manager::ManagerStartupTaskPolicy::
                        RequiredPriority),
        "the isolated task was not exact and disabled before the priority-drift mutation");
    require(
        settings->put_Priority(
            static_cast<int>(
                ForgeConductor::Manager::ManagerStartupTaskPolicy::
                    RequiredPriority) -
            1) == S_OK,
        "Task Scheduler refused the bounded priority-drift mutation");

    const auto userIdentity = take(Detail::strictUtf8ToUtf16(
        registration.definition.principal.userIdentity));
    auto userIdentityBstr = Detail::UniqueBstr::copy(userIdentity);
    require(
        static_cast<bool>(userIdentityBstr),
        "the registration user ID could not be allocated");
    Detail::UniqueVariant registrationUser;
    VARIANT* registrationUserValue = registrationUser.put();
    require(
        registrationUserValue != nullptr,
        "the registration user variant could not be prepared");
    registrationUserValue->vt = VT_BSTR;
    registrationUserValue->bstrVal = userIdentityBstr.detach();
    Detail::UniqueVariant registrationPassword;
    Detail::UniqueVariant securityDescriptor;
    Detail::UniqueComInterface<IRegisteredTask> updated;
    require(
        root->RegisterTaskDefinition(
            taskPath.get(),
            definition.get(),
            TASK_UPDATE | TASK_DISABLE,
            registrationUser.get(),
            registrationPassword.get(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            securityDescriptor.get(),
            updated.put()) == S_OK &&
            updated,
        "Task Scheduler could not persist the owned priority-drift mutation");
}

class OwnedTaskCleanup final {
public:
    OwnedTaskCleanup(
        Windows::WindowsManagerStartupService& service,
        const Domain::ManagerStartupDefinition& definition,
        const Detail::ManagerStartupResolvedRegistration& registration,
        OperationSequence& operations) noexcept
        : service_{service},
          definition_{definition},
          registration_{registration},
          operations_{operations}
    {
    }

    ~OwnedTaskCleanup()
    {
        cleanup();
    }

    OwnedTaskCleanup(const OwnedTaskCleanup&) = delete;
    OwnedTaskCleanup& operator=(const OwnedTaskCleanup&) = delete;

    void disarm() noexcept
    {
        armed_ = false;
    }

private:
    void fallbackCleanup() noexcept
    {
        switch (cleanupOwnedTaskDirectly(registration_)) {
        case DirectCleanupResult::Missing:
        case DirectCleanupResult::Removed:
            return;
        case DirectCleanupResult::Refused:
            std::cerr << "[CLEANUP REFUSED] direct ownership verification failed\n";
            return;
        case DirectCleanupResult::Failed:
            std::cerr << "[CLEANUP FAILURE] direct owned-task cleanup failed\n";
            return;
        }
    }

    void cleanup() noexcept
    {
        if (!armed_) {
            return;
        }
        try {
            auto observed = service_.inspect(definition_, operations_.next());
            if (!observed) {
                std::cerr << "[CLEANUP FAILURE] startup inspection failed: "
                          << observed.error().code << ": "
                          << observed.error().message << '\n';
                fallbackCleanup();
                return;
            }
            if (observed.value().state ==
                Domain::ManagerStartupState::ForeignConflict) {
                std::cerr << "[CLEANUP REFUSED] the isolated task is not Forge-owned\n";
                return;
            }
            if (!isOwnedState(observed.value().state)) {
                return;
            }
            auto removed = service_.remove(
                definition_, operations_.next());
            if (!removed ||
                removed.value().status.state !=
                    Domain::ManagerStartupState::Missing) {
                std::cerr << "[CLEANUP FAILURE] the Forge-owned isolated task was not removed\n";
                fallbackCleanup();
            }
        } catch (const std::exception& error) {
            std::cerr << "[CLEANUP FAILURE] " << error.what() << '\n';
            fallbackCleanup();
        } catch (...) {
            std::cerr << "[CLEANUP FAILURE] unknown exception\n";
            fallbackCleanup();
        }
    }

    Windows::WindowsManagerStartupService& service_;
    const Domain::ManagerStartupDefinition& definition_;
    const Detail::ManagerStartupResolvedRegistration& registration_;
    OperationSequence& operations_;
    bool armed_{true};
};

[[nodiscard]] Domain::ManagerStartupDefinition definition(
    const std::wstring& fixture,
    const std::wstring& home)
{
    const auto executableUtf8 = take(Detail::strictUtf16ToUtf8(fixture));
    const auto homeUtf8 = take(Detail::strictUtf16ToUtf8(home));
    return Domain::ManagerStartupDefinition{
        take(Domain::PathText::create(executableUtf8)),
        take(Domain::PathText::create(homeUtf8))};
}

void requireMissing(
    const Domain::ManagerStartupStatus& status,
    const std::string& operation)
{
    require(status.state == Domain::ManagerStartupState::Missing &&
                !status.registered && !status.enabled &&
                !status.definitionMatches,
            operation + " did not observe a missing startup task");
}

void requireExact(
    const Domain::ManagerStartupStatus& status,
    const bool enabled,
    const std::string& operation)
{
    require(status.state ==
                (enabled ? Domain::ManagerStartupState::Ready
                         : Domain::ManagerStartupState::Disabled) &&
                status.registered && status.enabled == enabled &&
                status.definitionMatches &&
                status.registrationIdentity.has_value() &&
                !status.registrationIdentity->empty(),
            operation + " did not observe the exact canonical startup task");
}

void requireDriftedDisabled(
    const Domain::ManagerStartupStatus& status,
    const std::string& operation)
{
    require(
        status.state == Domain::ManagerStartupState::Drifted &&
            status.registered && !status.enabled &&
            !status.definitionMatches &&
            status.registrationIdentity.has_value() &&
            !status.registrationIdentity->empty(),
        operation + " did not observe the owned disabled priority drift");
}

[[nodiscard]] bool sentinelMatches(const std::wstring& path) noexcept
{
    UniqueHandle file{::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr)};
    if (!file.valid()) {
        return false;
    }

    LARGE_INTEGER length{};
    if (!::GetFileSizeEx(file.get(), &length) || length.QuadPart < 0 ||
        static_cast<unsigned long long>(length.QuadPart) !=
            SentinelPayload.size()) {
        return false;
    }
    std::array<char, SentinelPayload.size()> bytes{};
    DWORD read{};
    return ::ReadFile(
               file.get(),
               bytes.data(),
               static_cast<DWORD>(bytes.size()),
               &read,
               nullptr) != FALSE &&
        read == bytes.size() &&
        std::string_view{bytes.data(), bytes.size()} == SentinelPayload;
}

void waitForSentinel(const std::wstring& path)
{
    const auto deadline = std::chrono::steady_clock::now() + SentinelTimeout;
    do {
        if (sentinelMatches(path)) {
            return;
        }
        std::this_thread::sleep_for(PollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    fail("the scheduled fixture did not write the expected sentinel before the deadline");
}

void waitUntilStopped(
    Windows::WindowsManagerStartupService& service,
    const Domain::ManagerStartupDefinition& expected,
    OperationSequence& operations)
{
    const auto deadline = std::chrono::steady_clock::now() + StopTimeout;
    do {
        const auto observed = take(service.inspect(expected, operations.next()));
        requireExact(observed, true, "post-start inspection");
        if (!observed.running) {
            return;
        }
        std::this_thread::sleep_for(PollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    fail("the scheduled fixture remained running past the bounded stop deadline");
}

void ensureInitialMissing(
    Windows::WindowsManagerStartupService& service,
    const Domain::ManagerStartupDefinition& expected,
    OperationSequence& operations)
{
    const auto initial = take(service.inspect(expected, operations.next()));
    if (initial.state == Domain::ManagerStartupState::ForeignConflict) {
        fail("the fixed isolated startup task is foreign-owned; refusing to delete it");
    }
    if (isOwnedState(initial.state)) {
        const auto removed = take(service.remove(expected, operations.next()));
        require(removed.changed, "the stale Forge-owned isolated task was not removed");
        requireMissing(removed.status, "stale-task cleanup");
    } else {
        requireMissing(initial, "initial inspection");
    }
    requireMissing(
        take(service.inspect(expected, operations.next())),
        "clean initial inspection");
}

void runLifecycle(const std::wstring& fixturePath)
{
    TemporaryHome home;
    const auto expected = definition(absolutePath(fixturePath), home.path());
    const auto identity = take(Windows::WindowsCurrentUserIdentity::load());
    const auto registration = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            expected, identity, PurposeSuffix));
    OperationSequence operations;

    Windows::WindowsManagerStartupServiceOptions options;
    options.purposeSuffix = std::string{PurposeSuffix};
    Windows::WindowsManagerStartupService service{std::move(options)};
    OwnedTaskCleanup cleanup{
        service, expected, registration, operations};

    ensureInitialMissing(service, expected, operations);

    const auto registered = take(
        service.registerAtLogon(expected, operations.next()));
    require(registered.changed, "registerAtLogon did not create the isolated task");
    requireExact(registered.status, true, "registerAtLogon");
    requireExact(
        take(service.inspect(expected, operations.next())),
        true,
        "registered inspection");

    const auto disabled = take(
        service.setEnabled(expected, false, operations.next()));
    require(disabled.changed, "setEnabled(false) did not change the isolated task");
    requireExact(disabled.status, false, "setEnabled(false)");
    requireExact(
        take(service.inspect(expected, operations.next())),
        false,
        "disabled inspection");

    driftOwnedTaskPriority(registration);
    requireDriftedDisabled(
        take(service.inspect(expected, operations.next())),
        "priority-drift inspection");
    const auto repaired = take(service.repair(expected, operations.next()));
    require(repaired.changed, "repair did not replace the owned drifted task");
    requireExact(repaired.status, false, "repair");
    requireExact(
        take(service.inspect(expected, operations.next())),
        false,
        "repaired disabled inspection");

    const auto enabled = take(
        service.setEnabled(expected, true, operations.next()));
    require(enabled.changed, "setEnabled(true) did not change the isolated task");
    requireExact(enabled.status, true, "setEnabled(true)");

    const auto started = take(service.startNow(expected, operations.next()));
    require(started.changed, "startNow did not start the isolated task");
    requireExact(started.status, true, "startNow");
    waitForSentinel(home.sentinel());
    waitUntilStopped(service, expected, operations);

    const auto removed = take(service.remove(expected, operations.next()));
    require(removed.changed, "remove did not delete the isolated task");
    requireMissing(removed.status, "remove");
    requireMissing(
        take(service.inspect(expected, operations.next())),
        "final inspection");
    cleanup.disarm();

    require(home.cleanup(), "the lifecycle test home could not be removed");
}

} // namespace

int wmain(const int argumentCount, wchar_t** const arguments)
{
    if (argumentCount != 2 || arguments == nullptr || arguments[1] == nullptr) {
        std::wcerr << L"Usage: manager-startup-lifecycle-tests <absolute-fixture-path>\n";
        return 2;
    }

    try {
        runLifecycle(arguments[1]);
        std::cout << "[PASS] manager_startup.real_machine.lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] manager_startup.real_machine.lifecycle: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "[FAIL] manager_startup.real_machine.lifecycle: unknown exception\n";
        return 1;
    }
}
