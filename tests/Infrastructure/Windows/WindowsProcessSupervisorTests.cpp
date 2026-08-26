#include "../TestSupport.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "Infrastructure/Windows/Detail/CommandLineBuilder.h"
#include "Infrastructure/Windows/Detail/ProcessLaunchObserver.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::SystemClock;
using Infrastructure::Windows::WindowsProcessSupervisor;
using Infrastructure::Windows::WindowsRuntimeDiagnostics;
using Infrastructure::Windows::Detail::CommandLineBuilder;
using Infrastructure::Windows::Detail::EnvironmentEntry;
using Infrastructure::Windows::Detail::IProcessLaunchObserver;
using Infrastructure::Windows::Detail::ProcessSupervisorTestAccess;
using namespace std::chrono_literals;

class TestHandle final {
public:
    TestHandle() noexcept = default;
    explicit TestHandle(HANDLE value) noexcept : value_{value} {}
    ~TestHandle() noexcept
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(value_));
        }
    }

    TestHandle(const TestHandle&) = delete;
    TestHandle& operator=(const TestHandle&) = delete;
    TestHandle(TestHandle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
    TestHandle& operator=(TestHandle&& other) noexcept
    {
        if (this != &other) {
            TestHandle replacement{std::exchange(other.value_, nullptr)};
            std::swap(value_, replacement.value_);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{};
};

class BlockingProcessLaunchObserver final : public IProcessLaunchObserver {
public:
    BlockingProcessLaunchObserver()
        : beforeReached_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          allowCreate_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          afterReached_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          allowContinue_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)}
    {
        require(beforeReached_.valid() && allowCreate_.valid() && afterReached_.valid() &&
                    allowContinue_.valid(),
                "CreateEventW failed for process launch observation");
    }

    void beforeCreateProcess() noexcept override
    {
        if (::SetEvent(beforeReached_.get()) == FALSE ||
            ::WaitForSingleObject(allowCreate_.get(), 30'000U) != WAIT_OBJECT_0) {
            callbackFailed_.store(true, std::memory_order_release);
        }
    }

    void afterCreateProcess() noexcept override
    {
        if (::SetEvent(afterReached_.get()) == FALSE ||
            ::WaitForSingleObject(allowContinue_.get(), 30'000U) != WAIT_OBJECT_0) {
            callbackFailed_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool waitBeforeCreate() const noexcept
    {
        return ::WaitForSingleObject(beforeReached_.get(), 15'000U) == WAIT_OBJECT_0;
    }

    [[nodiscard]] bool waitAfterCreate() const noexcept
    {
        return ::WaitForSingleObject(afterReached_.get(), 15'000U) == WAIT_OBJECT_0;
    }

    void allowCreateProcess() noexcept { static_cast<void>(::SetEvent(allowCreate_.get())); }
    void allowRunToContinue() noexcept { static_cast<void>(::SetEvent(allowContinue_.get())); }

    [[nodiscard]] bool callbackFailed() const noexcept
    {
        return callbackFailed_.load(std::memory_order_acquire);
    }

private:
    TestHandle beforeReached_;
    TestHandle allowCreate_;
    TestHandle afterReached_;
    TestHandle allowContinue_;
    std::atomic_bool callbackFailed_{};
};

class ProcessLaunchReleaseGuard final {
public:
    explicit ProcessLaunchReleaseGuard(BlockingProcessLaunchObserver& observer) noexcept
        : observer_{observer}
    {}

    ~ProcessLaunchReleaseGuard() noexcept
    {
        observer_.allowCreateProcess();
        observer_.allowRunToContinue();
    }

    ProcessLaunchReleaseGuard(const ProcessLaunchReleaseGuard&) = delete;
    ProcessLaunchReleaseGuard& operator=(const ProcessLaunchReleaseGuard&) = delete;
    ProcessLaunchReleaseGuard(ProcessLaunchReleaseGuard&&) = delete;
    ProcessLaunchReleaseGuard& operator=(ProcessLaunchReleaseGuard&&) = delete;

private:
    BlockingProcessLaunchObserver& observer_;
};

class ProcessSupervisorHarness final {
public:
    explicit ProcessSupervisorHarness(std::shared_ptr<IProcessLaunchObserver> launchObserver = {})
        : supervisor_{launchObserver
                          ? ProcessSupervisorTestAccess::create(budgets_, diagnostics_,
                                                                std::move(launchObserver))
                          : std::make_unique<WindowsProcessSupervisor>(budgets_, diagnostics_)}
    {}

    [[nodiscard]] WindowsProcessSupervisor& supervisor() noexcept { return *supervisor_; }

    [[nodiscard]] WindowsRuntimeDiagnostics& diagnostics() noexcept { return *diagnostics_; }

private:
    Domain::ResourceBudgets budgets_{
        Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB)};
    SystemClock clock_;
    std::shared_ptr<WindowsRuntimeDiagnostics> diagnostics_{
        std::make_shared<WindowsRuntimeDiagnostics>(clock_, budgets_)};
    std::unique_ptr<WindowsProcessSupervisor> supervisor_;
};

class EnvironmentVariableScope final {
public:
    EnvironmentVariableScope(std::wstring name, const std::wstring& value) : name_{std::move(name)}
    {
        const auto required = ::GetEnvironmentVariableW(name_.c_str(), nullptr, 0U);
        if (required > 0U) {
            std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
            const auto written = ::GetEnvironmentVariableW(name_.c_str(), buffer.data(), required);
            require(written < required, "GetEnvironmentVariableW failed while saving a test value");
            original_.emplace(buffer.data(), static_cast<std::size_t>(written));
        } else {
            require(::GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                    "GetEnvironmentVariableW failed while probing a test value");
        }
        require(::SetEnvironmentVariableW(name_.c_str(), value.c_str()),
                "SetEnvironmentVariableW failed for a test value");
    }

    ~EnvironmentVariableScope() noexcept
    {
        static_cast<void>(
            ::SetEnvironmentVariableW(name_.c_str(), original_ ? original_->c_str() : nullptr));
    }

    EnvironmentVariableScope(const EnvironmentVariableScope&) = delete;
    EnvironmentVariableScope& operator=(const EnvironmentVariableScope&) = delete;
    EnvironmentVariableScope(EnvironmentVariableScope&&) = delete;
    EnvironmentVariableScope& operator=(EnvironmentVariableScope&&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> original_;
};

[[nodiscard]] std::string utf16ToUtf8(const std::wstring_view value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const auto length = static_cast<int>(value.size());
    const auto required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                                nullptr, 0, nullptr, nullptr);
    require(required > 0, "WideCharToMultiByte sizing failed");
    std::string converted(static_cast<std::size_t>(required), '\0');
    require(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                  converted.data(), required, nullptr, nullptr) == required,
            "WideCharToMultiByte conversion failed");
    return converted;
}

[[nodiscard]] std::wstring fullPath(const std::wstring& input)
{
    const auto required = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    require(required > 0U, "GetFullPathNameW sizing failed");
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const auto written = ::GetFullPathNameW(input.c_str(), required, buffer.data(), nullptr);
    require(written > 0U && written < required, "GetFullPathNameW failed");
    return std::wstring{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] Domain::PathText pathText(const std::wstring& value)
{
    return take(Domain::PathText::create(utf16ToUtf8(value)));
}

class AuthorityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::WorkspaceAuthority
    create(const Domain::PathText& root, const bool shellEnabled, const bool grantExecute = true)
    {
        std::vector<Domain::FileAccess> grants{Domain::FileAccess::Read};
        if (grantExecute) {
            grants.push_back(Domain::FileAccess::Execute);
        }
        return take(
            issueAuthority(parse<Domain::AuthorityId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
                           parse<Domain::ProjectId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
                           parse<Domain::ClientId>("p06-process-test"), {root},
                           grantExecute ? Domain::FileAccess::Execute : Domain::FileAccess::Read,
                           std::move(grants), {}, shellEnabled, 1U));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority>
    authorityFor(const Domain::ProjectId&, const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The process test issuer only exposes its deterministic factory."));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority>
    narrow(const Contracts::WorkspaceAuthority&, const std::vector<Domain::PathText>&,
           const std::vector<Domain::FileAccess>&, bool, std::uint64_t,
           const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The process test issuer does not narrow capabilities."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath>
    authorize(const Contracts::WorkspaceAuthority&, const Domain::PathAuthorizationRequest&,
              const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The process test issuer does not issue path capabilities."));
    }
};

[[nodiscard]] Domain::OperationId operationId(const std::uint32_t index)
{
    std::array<char, 37> text{};
    const auto written =
        std::snprintf(text.data(), text.size(), "%08x-0000-4000-8000-%012x", index, index);
    require(written == 36, "operation UUID formatting failed");
    return parse<Domain::OperationId>(text.data());
}

[[nodiscard]] Domain::OperationContext context(const std::uint32_t index,
                                               const std::stop_token cancellation = {})
{
    return Domain::OperationContext{operationId(index), std::chrono::steady_clock::now() + 2min,
                                    cancellation,
                                    parse<Domain::CorrelationId>("p06-process-correlation")};
}

struct FixtureContext final {
    std::wstring executableWide;
    Domain::PathText executable;
    Domain::PathText root;
    Contracts::WorkspaceAuthority authority;

    explicit FixtureContext(const std::wstring& fixturePath)
        : executableWide{fullPath(fixturePath)}, executable{pathText(executableWide)},
          root{pathText(std::filesystem::path{executableWide}.parent_path().wstring())},
          authority{AuthorityIssuer::create(root, true)}
    {}

    [[nodiscard]] Domain::ProcessRequest request(std::vector<std::string> arguments) const
    {
        Domain::ProcessRequest request{executable};
        request.arguments = std::move(arguments);
        request.workingDirectory = root;
        request.timeout = 30s;
        return request;
    }
};

class TemporaryProcessTree final {
public:
    explicit TemporaryProcessTree(
        std::filesystem::path parent = std::filesystem::temp_directory_path())
    {
        LARGE_INTEGER counter{};
        require(::QueryPerformanceCounter(&counter) != FALSE,
                "QueryPerformanceCounter failed for the process path fixture");
        path_ = std::move(parent) /
                (L"ForgeConductor.ProcessPath." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                 std::to_wstring(counter.QuadPart));
        require(std::filesystem::create_directory(path_),
                "the process path fixture root could not be created");
    }

    ~TemporaryProcessTree() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    TemporaryProcessTree(const TemporaryProcessTree&) = delete;
    TemporaryProcessTree& operator=(const TemporaryProcessTree&) = delete;
    TemporaryProcessTree(TemporaryProcessTree&&) = delete;
    TemporaryProcessTree& operator=(TemporaryProcessTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::wstring uniqueEventName(const std::wstring_view suffix)
{
    LARGE_INTEGER counter{};
    static_cast<void>(::QueryPerformanceCounter(&counter));
    return L"Local\\ForgeConductor.ProcessTests." + std::to_wstring(::GetCurrentProcessId()) +
           L"." + std::to_wstring(counter.QuadPart) + L"." + std::wstring{suffix};
}

[[nodiscard]] TestHandle createEvent(const std::wstring& name)
{
    TestHandle event{::CreateEventW(nullptr, TRUE, FALSE, name.c_str())};
    require(event.valid(), "CreateEventW failed for process fixture synchronization");
    return event;
}

[[nodiscard]] std::string expectedEcho(const std::vector<std::string>& values)
{
    std::string expected;
    for (const auto& value : values) {
        expected += std::to_string(value.size()) + ":" + value + "\n";
    }
    return expected;
}

void testLaunchPinsAncestryOnlyThroughCreateProcess(const FixtureContext& fixture)
{
    TemporaryProcessTree tree;
    const auto executableParent = tree.path() / L"executable-parent";
    const auto workingParent = tree.path() / L"working-parent";
    const auto workingDirectory = workingParent / L"child";
    require(std::filesystem::create_directories(executableParent),
            "the executable ancestry fixture could not be created");
    require(std::filesystem::create_directories(workingDirectory),
            "the working-directory ancestry fixture could not be created");
    const auto executable = executableParent / L"ProcessFixture.exe";
    require(::CopyFileW(fixture.executableWide.c_str(), executable.c_str(), TRUE) != FALSE,
            "the process fixture executable could not be copied beneath the test authority");

    const auto renamedExecutableParent = tree.path() / L"renamed-executable-parent";
    const auto renamedWorkingParent = tree.path() / L"renamed-working-parent";
    require(::MoveFileExW(executableParent.c_str(), renamedExecutableParent.c_str(), 0U) != FALSE,
            "the executable-parent positive control was not renameable before launch");
    require(::MoveFileExW(renamedExecutableParent.c_str(), executableParent.c_str(), 0U) != FALSE,
            "the executable-parent positive control could not be restored");
    require(::MoveFileExW(workingParent.c_str(), renamedWorkingParent.c_str(), 0U) != FALSE,
            "the working-parent positive control was not renameable before launch");
    require(::MoveFileExW(renamedWorkingParent.c_str(), workingParent.c_str(), 0U) != FALSE,
            "the working-parent positive control could not be restored");

    const auto releaseName = uniqueEventName(L"ancestry-release");
    const auto readyName = uniqueEventName(L"ancestry-ready");
    auto release = createEvent(releaseName);
    auto ready = createEvent(readyName);

    Domain::ProcessRequest request{pathText(executable.wstring())};
    request.arguments = {"--wait-events", utf16ToUtf8(releaseName), utf16ToUtf8(readyName)};
    request.workingDirectory = pathText(workingDirectory.wstring());
    request.timeout = 30s;
    const auto root = pathText(tree.path().wstring());
    const auto authority = AuthorityIssuer::create(root, true);

    auto observer = std::make_shared<BlockingProcessLaunchObserver>();
    ProcessSupervisorHarness harness{observer};
    std::optional<Domain::Result<Domain::ProcessResult>> outcome;
    std::jthread runner{
        [&] { outcome.emplace(harness.supervisor().run(request, authority, context(31U))); }};
    ProcessLaunchReleaseGuard releaseObserver{*observer};
    const auto reachedBeforeCreate = observer->waitBeforeCreate();

    bool executableRenameBlocked{};
    DWORD executableRenameError{ERROR_SUCCESS};
    bool workingRenameBlocked{};
    DWORD workingRenameError{ERROR_SUCCESS};
    if (reachedBeforeCreate) {
        executableRenameBlocked =
            ::MoveFileExW(executableParent.c_str(), renamedExecutableParent.c_str(), 0U) == FALSE;
        executableRenameError = executableRenameBlocked ? ::GetLastError() : ERROR_SUCCESS;
        workingRenameBlocked =
            ::MoveFileExW(workingParent.c_str(), renamedWorkingParent.c_str(), 0U) == FALSE;
        workingRenameError = workingRenameBlocked ? ::GetLastError() : ERROR_SUCCESS;
    }

    require(reachedBeforeCreate,
            "the process launch observer did not reach the pre-CreateProcess boundary");
    require(executableRenameBlocked && (executableRenameError == ERROR_SHARING_VIOLATION ||
                                        executableRenameError == ERROR_ACCESS_DENIED),
            "the executable ancestry was renameable before CreateProcessW returned");
    require(workingRenameBlocked && (workingRenameError == ERROR_SHARING_VIOLATION ||
                                     workingRenameError == ERROR_ACCESS_DENIED),
            "the working-directory ancestry was renameable before CreateProcessW returned");

    observer->allowCreateProcess();
    const auto reachedAfterCreate = observer->waitAfterCreate();
    require(reachedAfterCreate,
            "the process launch observer did not reach the post-CreateProcess boundary");

    require(::MoveFileExW(executableParent.c_str(), renamedExecutableParent.c_str(), 0U) != FALSE,
            "the executable ancestry lease remained open after CreateProcessW returned");
    require(::MoveFileExW(renamedExecutableParent.c_str(), executableParent.c_str(), 0U) != FALSE,
            "the executable ancestry fixture could not be restored after launch");
    require(::MoveFileExW(workingParent.c_str(), renamedWorkingParent.c_str(), 0U) != FALSE,
            "the working-directory ancestry lease remained open after CreateProcessW returned");
    require(::MoveFileExW(renamedWorkingParent.c_str(), workingParent.c_str(), 0U) != FALSE,
            "the working-directory ancestry fixture could not be restored after launch");

    observer->allowRunToContinue();
    const DWORD readyWait = ::WaitForSingleObject(ready.get(), 15'000U);
    static_cast<void>(::SetEvent(release.get()));
    runner.join();

    require(readyWait == WAIT_OBJECT_0,
            "the process ancestry fixture did not reach its synchronized running state");
    require(!observer->callbackFailed(), "the process launch observer failed to synchronize");
    require(outcome.has_value() && outcome.value() && outcome->value().exitCode == 0,
            "the ancestry-pinned process did not complete successfully");
}

void testLaunchRejectsExecutableHardLinkOutsideAuthority(const FixtureContext& fixture)
{
    TemporaryProcessTree tree{std::filesystem::path{fixture.executableWide}.parent_path()};
    const auto linkedExecutable = tree.path() / L"outside-hard-link.exe";
    require(::CreateHardLinkW(linkedExecutable.c_str(), fixture.executableWide.c_str(), nullptr) !=
                FALSE,
            "the process hard-link authority fixture requires same-volume hard-link support");

    Domain::ProcessRequest request{pathText(linkedExecutable.wstring())};
    request.arguments = {"--exit", "0"};
    request.workingDirectory = pathText(tree.path().wstring());
    request.timeout = 10s;
    const auto root = pathText(tree.path().wstring());
    const auto authority = AuthorityIssuer::create(root, true);

    ProcessSupervisorHarness harness;
    const auto rejected = harness.supervisor().run(request, authority, context(32U));
    requireError(rejected, Domain::ErrorCodes::ProcessLaunchFailed,
                 "a hard-linked executable escaped process authority");
    require(std::filesystem::exists(fixture.executableWide),
            "hard-link rejection modified the out-of-authority executable");
}

void testCommandLineQuoting(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const std::vector<std::string> values{
        "plain", "", "with space", "trailing\\", "quote\"inside", "slashes\\\\\"quote"};
    auto arguments = std::vector<std::string>{"--echo"};
    arguments.insert(arguments.end(), values.begin(), values.end());
    const auto result =
        take(supervisor.run(fixture.request(std::move(arguments)), fixture.authority, context(1U)));
    require(result.exitCode == 0, "quoted fixture returned a nonzero status");
    require(result.stdoutUtf8 == expectedEcho(values),
            "CreateProcessW command-line quoting did not preserve argv");
}

void testEnvironmentAndWorkingDirectory(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    auto environmentRequest = fixture.request({"--environment", "FORGE_PROCESS_TEST_VALUE"});
    environmentRequest.inheritEnvironment = false;
    environmentRequest.environment.push_back({"FORGE_PROCESS_TEST_VALUE", "isolated-value"});
    const auto environment =
        take(supervisor.run(environmentRequest, fixture.authority, context(2U)));
    require(environment.stdoutUtf8 == "isolated-value",
            "explicit Unicode environment block did not reach the child");
    const auto systemRootRequired = ::GetEnvironmentVariableW(L"SystemRoot", nullptr, 0U);
    require(systemRootRequired > 1U, "SystemRoot was unavailable in the parent environment");
    std::vector<wchar_t> systemRootValue(static_cast<std::size_t>(systemRootRequired), L'\0');
    const auto systemRootWritten =
        ::GetEnvironmentVariableW(L"SystemRoot", systemRootValue.data(), systemRootRequired);
    require(systemRootWritten > 0U && systemRootWritten < systemRootRequired,
            "SystemRoot could not be read from the parent environment");
    const auto expectedSystemRoot = utf16ToUtf8(
        std::wstring_view{systemRootValue.data(), static_cast<std::size_t>(systemRootWritten)});

    const auto isolatedSystemRoot = take(supervisor.run(
        fixture.request({"--environment", "SystemRoot"}), fixture.authority, context(23U)));
    require(isolatedSystemRoot.exitCode == 5 && isolatedSystemRoot.stdoutUtf8.empty(),
            "default process launch leaked the ambient SystemRoot variable");

    EnvironmentVariableScope credentialCanary{L"FORGE_PROCESS_SECRET_TOKEN",
                                              L"must-not-reach-child"};
    auto inheritedSystemRootRequest = fixture.request({"--environment", "SystemRoot"});
    inheritedSystemRootRequest.inheritEnvironment = true;
    const auto inheritedSystemRoot =
        take(supervisor.run(inheritedSystemRootRequest, fixture.authority, context(24U)));
    require(inheritedSystemRoot.exitCode == 0 &&
                inheritedSystemRoot.stdoutUtf8 == expectedSystemRoot,
            "opt-in environment inheritance omitted allowlisted SystemRoot");

    auto inheritedPathRequest = fixture.request({"--environment", "PATH"});
    inheritedPathRequest.inheritEnvironment = true;
    const auto inheritedPath =
        take(supervisor.run(inheritedPathRequest, fixture.authority, context(25U)));
    require(inheritedPath.exitCode == 5 && inheritedPath.stdoutUtf8.empty(),
            "opt-in environment inheritance leaked non-allowlisted PATH");

    auto inheritedSecretRequest = fixture.request({"--environment", "FORGE_PROCESS_SECRET_TOKEN"});
    inheritedSecretRequest.inheritEnvironment = true;
    const auto inheritedSecret =
        take(supervisor.run(inheritedSecretRequest, fixture.authority, context(26U)));
    require(inheritedSecret.exitCode == 5 && inheritedSecret.stdoutUtf8.empty(),
            "opt-in environment inheritance leaked a credential marker");

    auto explicitOverrideRequest = fixture.request({"--environment", "TEMP"});
    explicitOverrideRequest.inheritEnvironment = true;
    explicitOverrideRequest.environment.push_back({"TeMp", "explicit-temp-override"});
    const auto explicitOverride =
        take(supervisor.run(explicitOverrideRequest, fixture.authority, context(27U)));
    require(explicitOverride.exitCode == 0 &&
                explicitOverride.stdoutUtf8 == "explicit-temp-override",
            "case-insensitive explicit environment override was not preserved");

    const auto workingDirectory = take(
        supervisor.run(fixture.request({"--working-directory"}), fixture.authority, context(3U)));
    const auto actualDirectory = take(CommandLineBuilder::utf8ToUtf16(workingDirectory.stdoutUtf8));
    const auto expectedDirectory =
        std::filesystem::path{fixture.executableWide}.parent_path().wstring();
    require(::CompareStringOrdinal(actualDirectory.data(), static_cast<int>(actualDirectory.size()),
                                   expectedDirectory.data(),
                                   static_cast<int>(expectedDirectory.size()), TRUE) == CSTR_EQUAL,
            "authorized working directory did not reach CreateProcessW");
}

void testOutputCapsAndConcurrentDrain(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    auto request = fixture.request({"--emit", "1048576", "1048576"});
    request.maximumStdoutBytes = 257U;
    request.maximumStderrBytes = 129U;
    request.timeout = 10s;
    const auto result = take(supervisor.run(request, fixture.authority, context(4U)));
    require(result.exitCode == 0, "heavy dual-stream fixture did not complete");
    require(result.stdoutUtf8.size() == 257U && result.stdoutTruncated,
            "stdout did not retain its independent exact cap");
    require(result.stderrUtf8.size() == 129U && result.stderrTruncated,
            "stderr did not retain its independent exact cap");
}

void testTimeoutAndNonzeroExit(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto nonzero =
        take(supervisor.run(fixture.request({"--exit", "37"}), fixture.authority, context(5U)));
    require(nonzero.exitCode == 37 && !nonzero.timedOut,
            "nonzero direct-child exit status was not preserved");

    auto request = fixture.request({"--sleep", "30000"});
    request.timeout = 50ms;
    const auto started = std::chrono::steady_clock::now();
    const auto timedOut = take(supervisor.run(request, fixture.authority, context(6U)));
    require(timedOut.timedOut && timedOut.exitCode != 0,
            "process timeout did not return a confirmed terminated result");
    require(std::chrono::steady_clock::now() - started < 2s,
            "process timeout exceeded its bounded termination path");

    auto deadlineRequest = fixture.request({"--sleep", "30000"});
    deadlineRequest.timeout = 10s;
    const Domain::OperationContext deadlineContext{
        operationId(22U), std::chrono::steady_clock::now() + 50ms, std::stop_token{},
        parse<Domain::CorrelationId>("p06-process-deadline")};
    const auto deadlineStarted = std::chrono::steady_clock::now();
    requireError(supervisor.run(deadlineRequest, fixture.authority, deadlineContext),
                 Domain::ErrorCodes::DeadlineExceeded,
                 "operation deadline did not terminate the child with a typed error");
    require(std::chrono::steady_clock::now() - deadlineStarted < 2s,
            "operation deadline exceeded its bounded termination path");
}

void testMalformedUtf8Replacement(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto result =
        take(supervisor.run(fixture.request({"--malformed"}), fixture.authority, context(7U)));
    require(result.stdoutUtf8 == "valid-\xef\xbf\xbdx",
            "malformed child output was not decoded with replacement");

    auto splitRequest = fixture.request({"--utf8-euro"});
    splitRequest.maximumStdoutBytes = 3U;
    const auto split = take(supervisor.run(splitRequest, fixture.authority, context(8U)));
    require(split.stdoutUtf8 == "A\xef\xbf\xbd",
            "a UTF-8 scalar split by the output cap was not replaced");
    require(split.stdoutTruncated,
            "a UTF-8 scalar split by the output cap did not mark truncation");
}

void testTargetedCancellation(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto releaseName = uniqueEventName(L"target.release");
    const auto firstReadyName = uniqueEventName(L"target.first");
    const auto secondReadyName = uniqueEventName(L"target.second");
    auto release = createEvent(releaseName);
    auto firstReady = createEvent(firstReadyName);
    auto secondReady = createEvent(secondReadyName);

    const auto firstContext = context(10U);
    const auto secondContext = context(11U);
    std::optional<Domain::Result<Domain::ProcessResult>> firstResult;
    std::optional<Domain::Result<Domain::ProcessResult>> secondResult;
    std::atomic_bool secondCompleted{};
    std::jthread first{[&] {
        firstResult.emplace(
            supervisor.run(fixture.request({"--wait-events", utf16ToUtf8(releaseName),
                                            utf16ToUtf8(firstReadyName)}),
                           fixture.authority, firstContext));
    }};
    std::jthread second{[&] {
        secondResult.emplace(
            supervisor.run(fixture.request({"--wait-events", utf16ToUtf8(releaseName),
                                            utf16ToUtf8(secondReadyName)}),
                           fixture.authority, secondContext));
        secondCompleted.store(true, std::memory_order_release);
    }};

    std::array<HANDLE, 2> ready{firstReady.get(), secondReady.get()};
    const auto readyWait =
        ::WaitForMultipleObjects(static_cast<DWORD>(ready.size()), ready.data(), TRUE, 10'000U);
    require(readyWait == WAIT_OBJECT_0, "targeted-cancellation fixtures did not become ready");
    supervisor.cancel(firstContext.operationId);
    first.join();
    require(firstResult.has_value() && firstResult.value(),
            "targeted cancellation did not return a process result");
    require(firstResult->value().cancelled,
            "targeted cancellation did not mark the selected operation");
    require(!secondCompleted.load(std::memory_order_acquire),
            "targeted cancellation terminated an unrelated operation");
    static_cast<void>(::SetEvent(release.get()));
    second.join();
    require(secondResult.has_value() && secondResult.value(),
            "unrelated operation failed after targeted cancellation");
    require(!secondResult->value().cancelled && secondResult->value().exitCode == 0,
            "unrelated operation was not independent of targeted cancellation");
}

void testShutdownCancelsAndRejects(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto releaseName = uniqueEventName(L"shutdown.release");
    const auto readyName = uniqueEventName(L"shutdown.ready");
    auto release = createEvent(releaseName);
    auto ready = createEvent(readyName);
    const auto activeContext = context(12U);
    std::optional<Domain::Result<Domain::ProcessResult>> result;
    std::jthread running{[&] {
        result.emplace(supervisor.run(
            fixture.request({"--wait-events", utf16ToUtf8(releaseName), utf16ToUtf8(readyName)}),
            fixture.authority, activeContext));
    }};
    require(::WaitForSingleObject(ready.get(), 10'000U) == WAIT_OBJECT_0,
            "shutdown fixture did not become ready");
    supervisor.shutdown();
    running.join();
    require(result.has_value() && result.value() && result->value().cancelled,
            "shutdown did not terminate and mark the active operation");
    requireError(supervisor.run(fixture.request({"--exit", "0"}), fixture.authority, context(13U)),
                 Domain::ErrorCodes::Cancelled, "shutdown supervisor admitted new work");
}

void testRuntimeOwnershipComposition(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto releaseName = uniqueEventName(L"ownership.release");
    const auto readyName = uniqueEventName(L"ownership.ready");
    auto release = createEvent(releaseName);
    auto ready = createEvent(readyName);
    const auto activeContext = context(28U);
    std::optional<Domain::Result<Domain::ProcessResult>> result;
    std::jthread running{[&] {
        result.emplace(supervisor.run(
            fixture.request({"--wait-events", utf16ToUtf8(releaseName), utf16ToUtf8(readyName)}),
            fixture.authority, activeContext));
    }};

    const auto readyWait = ::WaitForSingleObject(ready.get(), 10'000U);
    if (readyWait != WAIT_OBJECT_0) {
        supervisor.cancel(activeContext.operationId);
        running.join();
        require(false, "runtime-ownership fixture did not become ready");
    }
    const auto activeSnapshot = harness.diagnostics().snapshot(context(29U));
    supervisor.cancel(activeContext.operationId);
    running.join();

    require(static_cast<bool>(activeSnapshot),
            "runtime diagnostics could not snapshot an active process");
    require(activeSnapshot.value().childProcesses == 1U &&
                activeSnapshot.value().processReaders == 2U,
            "active process did not own one child and two reader leases");
    require(result.has_value() && result.value() && result->value().cancelled,
            "runtime-ownership fixture was not cancelled cleanly");
    const auto releasedSnapshot = take(harness.diagnostics().snapshot(context(30U)));
    require(releasedSnapshot.childProcesses == 0U && releasedSnapshot.processReaders == 0U,
            "process runtime ownership leases were not released");
}

void testActiveDestructorLifetime(const FixtureContext& fixture)
{
    const auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    SystemClock clock;
    WindowsProcessSupervisor missingDiagnostics{budgets, {}};
    requireError(
        missingDiagnostics.run(fixture.request({"--exit", "0"}), fixture.authority, context(32U)),
        Domain::ErrorCodes::InvalidRequest,
        "process supervisor accepted a missing runtime diagnostics owner");

    auto diagnostics = std::make_shared<WindowsRuntimeDiagnostics>(clock, budgets);
    std::weak_ptr<Contracts::IRuntimeDiagnostics> diagnosticsLifetime{diagnostics};
    auto supervisor = std::make_unique<WindowsProcessSupervisor>(budgets, diagnostics);
    auto* activeSupervisor = supervisor.get();
    diagnostics.reset();
    require(!diagnosticsLifetime.expired(),
            "supervisor did not retain shared runtime diagnostics ownership");

    const auto releaseName = uniqueEventName(L"destructor.release");
    const auto readyName = uniqueEventName(L"destructor.ready");
    auto release = createEvent(releaseName);
    auto ready = createEvent(readyName);
    const auto activeContext = context(31U);
    std::optional<Domain::Result<Domain::ProcessResult>> result;
    std::jthread running{[&] {
        result.emplace(activeSupervisor->run(
            fixture.request({"--wait-events", utf16ToUtf8(releaseName), utf16ToUtf8(readyName)}),
            fixture.authority, activeContext));
    }};

    const auto readyWait = ::WaitForSingleObject(ready.get(), 10'000U);
    if (readyWait != WAIT_OBJECT_0) {
        activeSupervisor->cancel(activeContext.operationId);
        running.join();
        require(false, "active-destructor fixture did not become ready");
    }
    supervisor.reset();
    running.join();

    require(result.has_value() && result.value() && result->value().cancelled,
            "supervisor destruction did not cancel active work safely");
    require(diagnosticsLifetime.expired(),
            "runtime diagnostics outlived the supervisor implementation owner");
}

void testDirectChildExitKillsDescendantWithoutEofWait(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto started = std::chrono::steady_clock::now();
    const auto result = take(
        supervisor.run(fixture.request({"--spawn-descendant"}), fixture.authority, context(14U)));
    require(result.exitCode == 0 && !result.timedOut, "direct child did not exit normally");
    require(std::chrono::steady_clock::now() - started < 2s,
            "supervisor waited for descendant pipe EOF");
    const auto descendantId = static_cast<DWORD>(std::stoul(result.stdoutUtf8));
    TestHandle descendant{
        ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, descendantId)};
    if (descendant.valid()) {
        require(::WaitForSingleObject(descendant.get(), 2'000U) == WAIT_OBJECT_0,
                "KILL_ON_JOB_CLOSE did not terminate the descendant tree");
    } else {
        require(::GetLastError() == ERROR_INVALID_PARAMETER,
                "descendant process could not be inspected after job termination");
    }
}

void testAuthorityAndLaunchFailures(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto withoutExecute = AuthorityIssuer::create(fixture.root, true, false);
    requireError(supervisor.run(fixture.request({"--exit", "0"}), withoutExecute, context(15U)),
                 Domain::ErrorCodes::Unauthorized, "authority without Execute launched a process");

    const auto disabled = AuthorityIssuer::create(fixture.root, false);
    requireError(supervisor.run(fixture.request({"--exit", "0"}), disabled, context(16U)),
                 Domain::ErrorCodes::ShellDisabled, "shell-disabled authority launched a process");

    wchar_t windowsDirectory[MAX_PATH]{};
    const auto windowsLength =
        ::GetWindowsDirectoryW(windowsDirectory, static_cast<UINT>(std::size(windowsDirectory)));
    require(windowsLength > 0U && windowsLength < std::size(windowsDirectory),
            "GetWindowsDirectoryW failed");
    const auto unrelatedRoot = pathText(std::wstring{windowsDirectory, windowsLength});
    const auto unrelatedAuthority = AuthorityIssuer::create(unrelatedRoot, true);
    requireError(supervisor.run(fixture.request({"--exit", "0"}), unrelatedAuthority, context(17U)),
                 Domain::ErrorCodes::Unauthorized,
                 "authority launched an executable outside its canonical root");

    auto outsideWorkingDirectory = fixture.request({"--exit", "0"});
    outsideWorkingDirectory.workingDirectory = unrelatedRoot;
    requireError(supervisor.run(outsideWorkingDirectory, fixture.authority, context(18U)),
                 Domain::ErrorCodes::Unauthorized,
                 "authority launched with a working directory outside its canonical root");

    auto relative = fixture.request({"--exit", "0"});
    relative.executable = take(Domain::PathText::create("ProcessFixture.exe"));
    requireError(supervisor.run(relative, fixture.authority, context(19U)),
                 Domain::ErrorCodes::InvalidRequest, "relative lpApplicationName was accepted");

    auto missing = fixture.request({"--exit", "0"});
    missing.executable = pathText(std::filesystem::path{fixture.executableWide}
                                      .parent_path()
                                      .append(L"missing-process-fixture.exe")
                                      .wstring());
    requireError(supervisor.run(missing, fixture.authority, context(20U)),
                 Domain::ErrorCodes::ProcessLaunchFailed,
                 "missing absolute executable did not return process_launch_failed");
    const auto recovery =
        take(supervisor.run(fixture.request({"--exit", "0"}), fixture.authority, context(21U)));
    require(recovery.exitCode == 0, "launch failure poisoned the next process operation");
}

void testLocalPathPreflight(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();

    auto uncExecutable = fixture.request({"--exit", "0"});
    uncExecutable.executable = pathText(L"\\\\localhost\\C$\\ForgeConductor\\ProcessFixture.exe");
    requireError(supervisor.run(uncExecutable, fixture.authority, context(33U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "UNC executable reached synchronous path authorization");

    auto uncWorkingDirectory = fixture.request({"--exit", "0"});
    uncWorkingDirectory.workingDirectory = pathText(L"//localhost/C$/ForgeConductor");
    requireError(supervisor.run(uncWorkingDirectory, fixture.authority, context(34U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "UNC working directory reached synchronous path authorization");

    auto deviceExecutable = fixture.request({"--exit", "0"});
    deviceExecutable.executable = pathText(L"\\\\?\\C:\\ForgeConductor\\ProcessFixture.exe");
    requireError(supervisor.run(deviceExecutable, fixture.authority, context(35U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "device-namespace executable reached synchronous path authorization");

    auto deviceWorkingDirectory = fixture.request({"--exit", "0"});
    deviceWorkingDirectory.workingDirectory = pathText(L"\\\\.\\C:\\ForgeConductor");
    requireError(supervisor.run(deviceWorkingDirectory, fixture.authority, context(36U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "device-namespace working directory reached synchronous path authorization");

    const auto disabled = AuthorityIssuer::create(fixture.root, false);
    requireError(supervisor.run(uncExecutable, disabled, context(37U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "UNC preflight did not precede authority evaluation");

    std::stop_source cancelled;
    require(cancelled.request_stop(), "path-preflight cancellation source did not stop");
    requireError(
        supervisor.run(uncExecutable, fixture.authority, context(38U, cancelled.get_token())),
        Domain::ErrorCodes::Cancelled, "cancelled UNC request performed local-path preflight");

    const Domain::OperationContext expiredContext{
        operationId(39U),
        std::chrono::steady_clock::now() - 1ms,
        {},
        parse<Domain::CorrelationId>("p06-process-correlation")};
    requireError(supervisor.run(uncExecutable, fixture.authority, expiredContext),
                 Domain::ErrorCodes::DeadlineExceeded,
                 "expired UNC request performed local-path preflight");

    supervisor.shutdown();
    requireError(supervisor.run(deviceExecutable, fixture.authority, context(40U)),
                 Domain::ErrorCodes::InvalidRequest,
                 "device-namespace preflight did not precede operation admission");
}

void testFinalNativeTextBounds()
{
    const std::wstring exactApplication =
        L"C:\\" +
        std::wstring(Domain::MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator - 4U, L'x');
    const auto exactCommand = CommandLineBuilder::buildCommandLine(exactApplication, {});
    require(exactCommand && exactCommand.value().size() ==
                                Domain::MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator,
            "exact native command-line ceiling was rejected");
    const std::wstring oversizedApplication = exactApplication + L"x";
    requireError(CommandLineBuilder::buildCommandLine(oversizedApplication, {}),
                 Domain::ErrorCodes::PayloadTooLarge,
                 "native command-line cap-plus-one was accepted");

    Domain::ProcessRequest request{take(Domain::PathText::create("C:\\fixture.exe"))};
    request.inheritEnvironment = true;
    const std::vector<EnvironmentEntry> exactEnvironment{
        {L"TEMP",
         std::wstring(Domain::MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators - 7U,
                      L'x')}};
    const auto exactBlock = CommandLineBuilder::buildEnvironmentBlock(request, exactEnvironment);
    require(exactBlock &&
                exactBlock.value().size() ==
                    Domain::MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators,
            "exact Forge environment-block ceiling was rejected");
    auto oversizedEnvironment = exactEnvironment;
    oversizedEnvironment.front().value.push_back(L'x');
    requireError(CommandLineBuilder::buildEnvironmentBlock(request, oversizedEnvironment),
                 Domain::ErrorCodes::PayloadTooLarge,
                 "Forge environment-block cap-plus-one was accepted");
}

void testHandleOwnershipStaysBounded(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    DWORD before{};
    require(::GetProcessHandleCount(::GetCurrentProcess(), &before),
            "GetProcessHandleCount failed before repeated runs");
    for (std::uint32_t index = 0U; index < 40U; ++index) {
        const auto result = take(supervisor.run(fixture.request({"--exit", "0"}), fixture.authority,
                                                context(100U + index)));
        require(result.exitCode == 0, "repeated fixture run failed");
    }
    DWORD after{};
    require(::GetProcessHandleCount(::GetCurrentProcess(), &after),
            "GetProcessHandleCount failed after repeated runs");
    require(after <= before + 2U, "repeated process runs retained native handles");
}

void testConcurrentAdmissionIsExactly64(const FixtureContext& fixture)
{
    ProcessSupervisorHarness harness;
    auto& supervisor = harness.supervisor();
    const auto releaseName = uniqueEventName(L"capacity.release");
    auto release = createEvent(releaseName);

    std::vector<TestHandle> readyEvents;
    std::vector<HANDLE> readyHandles;
    std::vector<std::wstring> readyNames;
    std::vector<std::optional<Domain::Result<Domain::ProcessResult>>> results(
        Domain::MaximumConcurrentProcessOperations);
    std::vector<std::jthread> runners;
    readyEvents.reserve(Domain::MaximumConcurrentProcessOperations);
    readyHandles.reserve(Domain::MaximumConcurrentProcessOperations);
    readyNames.reserve(Domain::MaximumConcurrentProcessOperations);
    runners.reserve(Domain::MaximumConcurrentProcessOperations);

    for (std::size_t index = 0U; index < Domain::MaximumConcurrentProcessOperations; ++index) {
        readyNames.push_back(uniqueEventName(L"capacity." + std::to_wstring(index)));
        readyEvents.push_back(createEvent(readyNames.back()));
        readyHandles.push_back(readyEvents.back().get());
        runners.emplace_back([&, index] {
            results[index].emplace(supervisor.run(
                fixture.request(
                    {"--wait-events", utf16ToUtf8(releaseName), utf16ToUtf8(readyNames[index])}),
                fixture.authority, context(static_cast<std::uint32_t>(1'000U + index))));
        });
    }

    const auto ready = ::WaitForMultipleObjects(static_cast<DWORD>(readyHandles.size()),
                                                readyHandles.data(), TRUE, 30'000U);
    if (ready != WAIT_OBJECT_0) {
        static_cast<void>(::SetEvent(release.get()));
        for (auto& runner : runners) {
            runner.join();
        }
        require(false, "64 admitted process fixtures did not become ready");
    }

    const auto overflowReadyName = uniqueEventName(L"capacity.overflow");
    const auto overflow = supervisor.run(fixture.request({"--wait-events", utf16ToUtf8(releaseName),
                                                          utf16ToUtf8(overflowReadyName)}),
                                         fixture.authority, context(2'000U));
    requireError(overflow, Domain::ErrorCodes::LimitExceeded,
                 "65th process operation was queued or admitted");

    static_cast<void>(::SetEvent(release.get()));
    for (auto& runner : runners) {
        runner.join();
    }
    for (const auto& result : results) {
        require(result.has_value() && result.value() && result->value().exitCode == 0,
                "one of 64 admitted process operations failed");
    }
}

} // namespace

void registerProcessWindowsTests(TestRegistry& tests, const std::wstring& fixturePath)
{
    const FixtureContext fixture{fixturePath};
    addTest(tests, "process.command_line_quoting", [fixture] { testCommandLineQuoting(fixture); });
    addTest(tests, "process.environment_and_working_directory",
            [fixture] { testEnvironmentAndWorkingDirectory(fixture); });
    addTest(tests, "process.launch-path-ancestry",
            [fixture] { testLaunchPinsAncestryOnlyThroughCreateProcess(fixture); });
    addTest(tests, "process.launch-hard-link-denial",
            [fixture] { testLaunchRejectsExecutableHardLinkOutsideAuthority(fixture); });
    addTest(tests, "process.independent_output_caps",
            [fixture] { testOutputCapsAndConcurrentDrain(fixture); });
    addTest(tests, "process.timeout_and_exit_status",
            [fixture] { testTimeoutAndNonzeroExit(fixture); });
    addTest(tests, "process.malformed_utf8_replacement",
            [fixture] { testMalformedUtf8Replacement(fixture); });
    addTest(tests, "process.targeted_cancellation",
            [fixture] { testTargetedCancellation(fixture); });
    addTest(tests, "process.shutdown", [fixture] { testShutdownCancelsAndRejects(fixture); });
    addTest(tests, "process.runtime_ownership",
            [fixture] { testRuntimeOwnershipComposition(fixture); });
    addTest(tests, "process.active_destructor_lifetime",
            [fixture] { testActiveDestructorLifetime(fixture); });
    addTest(tests, "process.descendant_job_termination",
            [fixture] { testDirectChildExitKillsDescendantWithoutEofWait(fixture); });
    addTest(tests, "process.authority_and_launch_failures",
            [fixture] { testAuthorityAndLaunchFailures(fixture); });
    addTest(tests, "process.local_path_preflight", [fixture] { testLocalPathPreflight(fixture); });
    addTest(tests, "process.final_native_text_bounds", [] { testFinalNativeTextBounds(); });
    addTest(tests, "process.handle_ownership",
            [fixture] { testHandleOwnershipStaysBounded(fixture); });
    addTest(tests, "process.concurrent_admission_64_65",
            [fixture] { testConcurrentAdmissionIsExactly64(fixture); });
}

} // namespace ForgeConductor::Tests
