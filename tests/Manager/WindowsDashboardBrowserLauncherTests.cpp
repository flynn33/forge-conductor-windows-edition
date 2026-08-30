#include "Infrastructure/TestSupport.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/IWindowsDashboardUriLaunchPlatform.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBrowserLauncher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardUriActivationCommand.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct WindowsDashboardBrowserLauncherTestAccess final {
    [[nodiscard]] static bool closed(
        const WindowsDashboardBrowserLauncher& launcher) noexcept
    {
        return launcher.closedForTesting();
    }

    [[nodiscard]] static bool waitUntilJoinOwned(
        WindowsDashboardBrowserLauncher& launcher,
        const std::chrono::milliseconds timeout) noexcept
    {
        return launcher.waitUntilJoinOwnedForTesting(timeout);
    }
};

struct WindowsDashboardUriActivationCommandTestAccess final {
    [[nodiscard]] static std::unique_ptr<WindowsDashboardUriActivationCommand>
    create(std::unique_ptr<IWindowsDashboardUriLaunchPlatform> platform)
    {
        return std::unique_ptr<WindowsDashboardUriActivationCommand>{
            new WindowsDashboardUriActivationCommand{std::move(platform)}};
    }
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsDashboardBrowserLaunchConfiguration;
using Infrastructure::Windows::WindowsDashboardBrowserLauncher;
using Infrastructure::Windows::WindowsDashboardUriActivationCommand;
using Infrastructure::Windows::Detail::IWindowsDashboardUriLaunchPlatform;
using Infrastructure::Windows::Detail::
    WindowsDashboardBrowserLauncherTestAccess;
using Infrastructure::Windows::Detail::
    WindowsDashboardUriActivationCommandTestAccess;
using namespace std::chrono_literals;

static_assert(std::is_final_v<WindowsDashboardBrowserLauncher>);
static_assert(!std::is_copy_constructible_v<WindowsDashboardBrowserLauncher>);
static_assert(!std::is_move_constructible_v<WindowsDashboardBrowserLauncher>);
static_assert(std::is_final_v<WindowsDashboardUriActivationCommand>);

constexpr std::string_view Token{
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
constexpr std::string_view Ipv4Uri{
    "http://127.0.0.1:7788/#token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
constexpr std::string_view Ipv6Uri{
    "http://[::1]:65535/#token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};

[[noreturn]] void fail(const std::string_view message)
{
    throw TestFailure{std::string{message}};
}

class FakeClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utcNow_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept
        override
    {
        return monotonicNow_;
    }

    Domain::UtcTimePoint utcNow_{};
    Domain::MonotonicTimePoint monotonicNow_{
        std::chrono::steady_clock::now()};
};

class RecordingDiagnostics final : public Contracts::IDiagnosticSink {
public:
    [[nodiscard]] Domain::Result<void> record(
        const Domain::DiagnosticEnvelope& event,
        const Domain::OperationContext&) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++recordAttempts_;
        }
        changed_.notify_all();
        const std::lock_guard serializationLock{recordSerializationMutex_};
        std::function<void()> hook;
        {
            const std::lock_guard lock{mutex_};
            events_.push_back(event);
            hook = hook_;
        }
        changed_.notify_all();
        if (hook) {
            hook();
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        const std::lock_guard lock{mutex_};
        const auto count = (std::min)(maximumCount, events_.size());
        return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(
            std::vector<Domain::DiagnosticEnvelope>{
                events_.end() - static_cast<std::ptrdiff_t>(count),
                events_.end()});
    }

    [[nodiscard]] Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest&,
        const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DiagnosticExportResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "Diagnostic export is unavailable in this test."));
    }

    void shutdown() noexcept override {}

    void setHook(std::function<void()> hook)
    {
        const std::lock_guard lock{mutex_};
        hook_ = std::move(hook);
    }

    [[nodiscard]] bool waitUntilCount(
        const std::size_t expected,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout,
            [&]() noexcept { return events_.size() >= expected; });
    }

    [[nodiscard]] bool waitUntilAttempts(
        const std::size_t expected,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout,
            [&]() noexcept { return recordAttempts_ >= expected; });
    }

    [[nodiscard]] std::vector<Domain::DiagnosticEnvelope> events() const
    {
        const std::lock_guard lock{mutex_};
        return events_;
    }

private:
    std::mutex recordSerializationMutex_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Domain::DiagnosticEnvelope> events_;
    std::function<void()> hook_;
    std::size_t recordAttempts_{};
};

class RecordingProcessSupervisor final
    : public Contracts::IProcessSupervisor {
public:
    struct Captured final {
        Domain::ProcessRequest request;
        Contracts::WorkspaceAuthority authority;
        Domain::OperationContext context;
    };

    void failWith(Domain::Error error)
    {
        const std::lock_guard lock{mutex_};
        failure_ = std::move(error);
    }

    void ignoreCancellationUntilReleased(const bool value) noexcept
    {
        const std::lock_guard lock{mutex_};
        ignoreCancellation_ = value;
    }

    void release() noexcept
    {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool waitUntilRun(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this]() noexcept { return entered_; });
    }

    [[nodiscard]] bool waitUntilReturned(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this]() noexcept { return returned_; });
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::stop_callback cancelled{
                context.cancellation,
                [this]() noexcept { changed_.notify_all(); }};
            std::optional<Domain::Error> failure;
            Domain::ProcessResult result;
            bool wasCancelled{};
            {
                std::unique_lock lock{mutex_};
                ++runCalls_;
                ++activeCalls_;
                captured_.emplace(Captured{request, authority, context});
                entered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this, &context]() noexcept {
                    return released_ ||
                           (!ignoreCancellation_ &&
                            (cancelRequested_ ||
                             context.isCancellationRequested()));
                });
                wasCancelled =
                    cancelRequested_ || context.isCancellationRequested();
                failure = failure_;
                result = result_;
                --activeCalls_;
                returned_ = true;
            }
            changed_.notify_all();
            if (wasCancelled) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The recording helper was cancelled."));
            }
            if (failure) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(*failure));
            }
            return Domain::Result<Domain::ProcessResult>::success(
                std::move(result));
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording process supervisor failed."));
        }
    }

    void cancel(const Domain::OperationId&) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++cancelCalls_;
            cancelRequested_ = true;
        }
        changed_.notify_all();
    }

    void cancelAll() noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            cancelRequested_ = true;
        }
        changed_.notify_all();
    }

    void shutdown() noexcept override { cancelAll(); }

    [[nodiscard]] Captured captured() const
    {
        const std::lock_guard lock{mutex_};
        if (!captured_) {
            throw TestFailure{"No helper process request was captured."};
        }
        return *captured_;
    }

    [[nodiscard]] std::size_t runCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return runCalls_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] std::size_t activeCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return activeCalls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Captured> captured_;
    std::optional<Domain::Error> failure_;
    Domain::ProcessResult result_{};
    std::size_t runCalls_{};
    std::size_t cancelCalls_{};
    std::size_t activeCalls_{};
    bool entered_{};
    bool returned_{};
    bool released_{};
    bool cancelRequested_{};
    bool ignoreCancellation_{};
};

[[nodiscard]] WindowsDashboardBrowserLaunchConfiguration configuration(
    const Domain::OperationContext& context,
    const std::string_view helperLeaf = "forge-conductor.exe",
    std::vector<Domain::FileAccess> denials = {})
{
    const auto root = take(Domain::PathText::create("C:\\Forge\\bin"));
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>(
            "c2000000-0000-4000-8000-000000000001"),
        parse<Domain::ClientId>("manager-browser-helper"),
        {root},
        Domain::FileAccess::Execute,
        {Domain::FileAccess::Execute},
        std::move(denials),
        true,
        1U};
    auto authority = take(issuer.authorityFor(
        parse<Domain::ProjectId>(
            "c2000000-0000-4000-8000-000000000002"),
        context));
    return WindowsDashboardBrowserLaunchConfiguration{
        take(Domain::PathText::create(
            "C:\\Forge\\bin\\ForgeConductor.Manager.exe")),
        take(Domain::PathText::create(
            std::string{"C:\\Forge\\bin\\"} +
            std::string{helperLeaf})),
        std::move(authority),
        2s};
}

struct Fixture final {
    Fixture()
        : supervisor{std::make_shared<RecordingProcessSupervisor>()}
    {
        launcher = std::make_unique<WindowsDashboardBrowserLauncher>(
            clock,
            diagnostics,
            supervisor,
            configuration(context()),
            parse<Domain::Sha256Digest>(Token));
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::stop_token cancellation = {}) const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>(
                "c1000000-0000-4000-8000-000000000001"),
            clock.monotonicNow_ + 5min,
            cancellation,
            parse<Domain::CorrelationId>(
                "manager-browser-launcher-test")};
    }

    FakeClock clock;
    RecordingDiagnostics diagnostics;
    std::shared_ptr<RecordingProcessSupervisor> supervisor;
    std::unique_ptr<WindowsDashboardBrowserLauncher> launcher;
};

class RecordingUriPlatform final
    : public IWindowsDashboardUriLaunchPlatform {
public:
    void failWith(Domain::Error error)
    {
        failure_ = std::move(error);
    }

    [[nodiscard]] Domain::Result<void> open(
        const std::wstring_view uri) noexcept override
    {
        ++calls_;
        uri_ = uri;
        if (failure_) {
            return Domain::Result<void>::failure(*failure_);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] const std::wstring& uri() const noexcept { return uri_; }

private:
    std::optional<Domain::Error> failure_;
    std::wstring uri_;
    std::size_t calls_{};
};

[[nodiscard]] std::string strictWideToUtf8(const std::wstring_view value)
{
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0, "wide helper path conversion size");
    std::string converted(static_cast<std::size_t>(required), '\0');
    require(::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), converted.data(), required,
                nullptr, nullptr) == required,
            "wide helper path conversion");
    return converted;
}

void realHelperRejectsOversizedStdinAtProcessBoundary(
    const std::wstring_view helperPath)
{
    const std::filesystem::path absolute =
        std::filesystem::absolute(std::filesystem::path{helperPath});
    const auto executable = take(Domain::PathText::create(
        strictWideToUtf8(absolute.wstring())));
    const auto root = take(Domain::PathText::create(
        strictWideToUtf8(absolute.parent_path().wstring())));
    const Domain::OperationContext operation{
        parse<Domain::OperationId>(
            "c3000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + 30s,
        {},
        parse<Domain::CorrelationId>("browser-helper-process-test")};
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>(
            "c3000000-0000-4000-8000-000000000002"),
        parse<Domain::ClientId>("browser-helper-process"),
        {root},
        Domain::FileAccess::Execute,
        {Domain::FileAccess::Execute},
        {},
        true,
        1U};
    const auto authority = take(issuer.authorityFor(
        parse<Domain::ProjectId>(
            "c3000000-0000-4000-8000-000000000003"),
        operation));

    const auto budgets = Domain::budgetsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    Infrastructure::Windows::SystemClock clock;
    auto diagnostics = std::make_shared<
        Infrastructure::Windows::WindowsRuntimeDiagnostics>(clock, budgets);
    Infrastructure::Windows::WindowsProcessSupervisor supervisor{
        budgets, diagnostics};
    Domain::ProcessRequest request{
        executable,
        {"--internal-open-dashboard-uri"},
        root,
        {},
        true,
        5s,
        1'024U,
        1'024U,
        std::string(
            WindowsDashboardUriActivationCommand::MaximumUriBytes + 1U,
            'x')};
    const auto result = supervisor.run(request, authority, operation);
    supervisor.shutdown();

    require(result.hasValue(), "real helper process did not return a result");
    require(result.value().exitCode == 1 &&
                result.value().terminationConfirmed &&
                !result.value().timedOut && !result.value().cancelled,
            "real helper did not reject oversized stdin deterministically");
    require(result.value().stdoutUtf8.empty(),
            "real helper wrote unexpected standard output");
    require(result.value().stderrUtf8.find(
                Domain::ErrorCodes::PayloadTooLarge) != std::string::npos &&
                result.value().stderrUtf8.find(Token) == std::string::npos &&
                result.value().stderrUtf8.find("#token=") ==
                    std::string::npos &&
                result.value().stderrUtf8.find(std::string(16U, 'x')) ==
                    std::string::npos,
            "real helper output did not retain only the typed secret-free failure");
}

void admissionIsPromptAndHelperRequestIsSecretSafe()
{
    Fixture fixture;
    const auto started = std::chrono::steady_clock::now();
    require(fixture.launcher->launch(
                "127.0.0.1", 7788U, fixture.context()).hasValue(),
            "dashboard browser work was not admitted");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!fixture.supervisor->waitUntilRun(2s)) {
        fixture.supervisor->release();
        fixture.launcher->shutdown();
        fail("dashboard helper process entry");
    }

    const auto captured = fixture.supervisor->captured();
    const bool prompt = elapsed < 100ms;
    const bool exactExecutable = captured.request.executable.value() ==
        "C:\\Forge\\bin\\forge-conductor.exe";
    const bool exactArguments = captured.request.arguments.size() == 1U &&
        captured.request.arguments.front() ==
            "--internal-open-dashboard-uri";
    const bool exactStdin = captured.request.stdinUtf8 == Ipv4Uri;
    const bool exactWorkingDirectory =
        captured.request.workingDirectory.has_value() &&
        captured.request.workingDirectory->value() == "C:\\Forge\\bin";
    const bool bounded = captured.request.timeout == 2s &&
        captured.request.maximumStdoutBytes == 1'024U &&
        captured.request.maximumStderrBytes == 1'024U;
    const bool noSecretInArguments = std::none_of(
        captured.request.arguments.begin(),
        captured.request.arguments.end(),
        [](const std::string& argument) {
            return argument.find(Token) != std::string::npos;
        });
    const bool noExplicitEnvironment = captured.request.environment.empty();
    const bool exactAuthority =
        captured.authority.trustedRoots().size() == 1U &&
        captured.authority.trustedRoots().front().value() ==
            "C:\\Forge\\bin" &&
        captured.authority.intent() == Domain::FileAccess::Execute &&
        captured.authority.shellEnabled() &&
        captured.authority.grants().size() == 1U &&
        captured.authority.grants().front() == Domain::FileAccess::Execute &&
        captured.authority.denials().empty();
    const auto second = fixture.launcher->launch(
        "127.0.0.1", 7788U, fixture.context());

    fixture.supervisor->release();
    const bool returned = fixture.supervisor->waitUntilReturned(2s);
    fixture.launcher->shutdown();
    const auto events = fixture.diagnostics.events();
    const bool conflictRecorded = std::ranges::any_of(
        events,
        [](const Domain::DiagnosticEnvelope& event) {
            return event.event ==
                       "manager_dashboard_browser_open_failed" &&
                event.fields.size() == 1U &&
                event.fields.front().value == Domain::ErrorCodes::Conflict;
        });

    require(prompt, "helper admission synchronously held Manager startup");
    require(exactExecutable, "helper executable was not the exact CLI sibling");
    require(exactArguments, "helper mode argv was not exact");
    require(exactStdin, "authenticated URI was not carried through exact stdin");
    require(exactWorkingDirectory,
            "helper did not use its exact authorized parent as working directory");
    require(bounded, "helper process request was not bounded");
    require(noSecretInArguments && noExplicitEnvironment,
            "dashboard bearer escaped stdin into argv or environment");
    require(exactAuthority, "helper execution authority was wider than exact execute scope");
    requireError(second, Domain::ErrorCodes::Conflict,
                 "capacity-one launcher admitted a second operation");
    require(conflictRecorded,
            "capacity-one rejection was not diagnosed outside the state lock");
    require(returned, "helper process did not return after release");
    require(WindowsDashboardBrowserLauncherTestAccess::closed(*fixture.launcher),
            "exact shutdown did not publish final Closed state");
}

void endpointContextAndConfigurationRejectBeforeProcessAdmission()
{
    {
        Fixture fixture;
        const auto rejected = fixture.launcher->launch(
            "localhost", 7788U, fixture.context());
        const bool recorded = fixture.diagnostics.waitUntilCount(1U, 2s);
        fixture.launcher->shutdown();
        requireError(rejected, Domain::ErrorCodes::InvalidRequest,
                     "noncanonical dashboard host was accepted");
        require(recorded && fixture.diagnostics.events().front().fields.front().value ==
                    Domain::ErrorCodes::InvalidRequest,
                "immediate endpoint rejection was not diagnosed asynchronously");
        require(fixture.supervisor->runCalls() == 0U,
                "rejected endpoint reached the process boundary");
    }
    {
        Fixture fixture;
        const auto rejected = fixture.launcher->launch(
            "127.0.0.1", 0U, fixture.context());
        fixture.launcher->shutdown();
        requireError(rejected, Domain::ErrorCodes::InvalidRequest,
                     "zero dashboard port was accepted");
        require(fixture.supervisor->runCalls() == 0U,
                "zero-port rejection reached the process boundary");
    }
    {
        Fixture fixture;
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto rejected = fixture.launcher->launch(
            "127.0.0.1", 7788U,
            fixture.context(cancellation.get_token()));
        fixture.launcher->shutdown();
        requireError(rejected, Domain::ErrorCodes::Cancelled,
                     "cancelled dashboard launch was admitted");
        require(fixture.supervisor->runCalls() == 0U,
                "cancelled launch reached the process boundary");
    }
    {
        Fixture fixture;
        const Domain::OperationContext expired{
            parse<Domain::OperationId>(
                "c1000000-0000-4000-8000-000000000002"),
            fixture.clock.monotonicNow_,
            {},
            parse<Domain::CorrelationId>("browser-expired")};
        const auto rejected = fixture.launcher->launch(
            "127.0.0.1", 7788U, expired);
        fixture.launcher->shutdown();
        requireError(rejected, Domain::ErrorCodes::DeadlineExceeded,
                     "expired dashboard launch was admitted");
        require(fixture.supervisor->runCalls() == 0U,
                "expired launch reached the process boundary");
    }

    FakeClock clock;
    RecordingDiagnostics diagnostics;
    auto supervisor = std::make_shared<RecordingProcessSupervisor>();
    WindowsDashboardBrowserLauncher invalid{
        clock,
        diagnostics,
        supervisor,
        configuration(
            Domain::OperationContext{
                parse<Domain::OperationId>(
                    "c1000000-0000-4000-8000-000000000003"),
                clock.monotonicNow_ + 5min,
                {},
                parse<Domain::CorrelationId>("browser-invalid-config")},
            "renamed-helper.exe"),
        parse<Domain::Sha256Digest>(Token)};
    const auto invalidResult = invalid.launch(
            "127.0.0.1", 7788U,
            Domain::OperationContext{
                parse<Domain::OperationId>(
                    "c1000000-0000-4000-8000-000000000004"),
                clock.monotonicNow_ + 5min,
                {},
                parse<Domain::CorrelationId>("browser-invalid-launch")});
    const bool invalidRecorded = diagnostics.waitUntilCount(1U, 2s);
    invalid.shutdown();
    requireError(invalidResult, Domain::ErrorCodes::IntegrityFailure,
                 "renamed helper executable was accepted");
    require(invalidRecorded,
            "immediate configuration rejection was not diagnosed asynchronously");
    require(supervisor->runCalls() == 0U,
            "invalid helper configuration reached the process boundary");

    WindowsDashboardBrowserLauncher denied{
        clock,
        diagnostics,
        supervisor,
        configuration(
            Domain::OperationContext{
                parse<Domain::OperationId>(
                    "c1000000-0000-4000-8000-000000000005"),
                clock.monotonicNow_ + 5min,
                {},
                parse<Domain::CorrelationId>("browser-denied-config")},
            "forge-conductor.exe",
            {Domain::FileAccess::Read}),
        parse<Domain::Sha256Digest>(Token)};
    requireError(
        denied.launch(
            "127.0.0.1", 7788U,
            Domain::OperationContext{
                parse<Domain::OperationId>(
                    "c1000000-0000-4000-8000-000000000006"),
                clock.monotonicNow_ + 5min,
                {},
                parse<Domain::CorrelationId>("browser-denied-launch")}),
        Domain::ErrorCodes::IntegrityFailure,
        "helper authority with an unexpected denial was accepted");
    denied.shutdown();
    require(supervisor->runCalls() == 0U,
            "non-exact helper authority reached the process boundary");
}

void immediateDiagnosticIsNonblockingAndExactlyJoined()
{
    Fixture fixture;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool diagnosticEntered{};
    bool releaseDiagnostic{};
    bool shutdownReturned{};
    fixture.diagnostics.setHook([&]() {
        {
            const std::lock_guard lock{gateMutex};
            diagnosticEntered = true;
        }
        gateChanged.notify_all();
        std::unique_lock lock{gateMutex};
        gateChanged.wait(
            lock, [&]() noexcept { return releaseDiagnostic; });
    });

    const auto started = std::chrono::steady_clock::now();
    const auto rejected = fixture.launcher->launch(
        "localhost", 7788U, fixture.context());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    bool entered{};
    {
        std::unique_lock lock{gateMutex};
        entered = gateChanged.wait_for(
            lock, 2s, [&]() noexcept { return diagnosticEntered; });
    }

    std::jthread shuttingDown{[&]() noexcept {
        fixture.launcher->shutdown();
        {
            const std::lock_guard lock{gateMutex};
            shutdownReturned = true;
        }
        gateChanged.notify_all();
    }};
    bool returnedEarly{};
    {
        std::unique_lock lock{gateMutex};
        returnedEarly = gateChanged.wait_for(
            lock, 100ms, [&]() noexcept { return shutdownReturned; });
        releaseDiagnostic = true;
    }
    gateChanged.notify_all();
    shuttingDown.join();

    requireError(rejected, Domain::ErrorCodes::InvalidRequest,
                 "blockable diagnostic endpoint was accepted");
    require(elapsed < 100ms,
            "blockable diagnostic sink held browser launch admission");
    require(entered,
            "immediate diagnostic did not reach its owned executor");
    require(!returnedEarly,
            "exact shutdown returned before the diagnostic owner drained");
    require(WindowsDashboardBrowserLauncherTestAccess::closed(*fixture.launcher),
            "diagnostic executor did not publish exact Closed state");
}

void crossWorkerRecursiveShutdownDefersBothExactJoins()
{
    Fixture fixture;
    fixture.supervisor->failWith(Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "The scripted helper boundary failed."));

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool launchDiagnosticEntered{};
    bool allowRecursiveShutdown{};
    bool recursiveShutdownReturned{};
    fixture.diagnostics.setHook([&]() {
        bool firstDiagnostic{};
        {
            std::unique_lock lock{gateMutex};
            if (!launchDiagnosticEntered) {
                launchDiagnosticEntered = true;
                firstDiagnostic = true;
                gateChanged.notify_all();
                gateChanged.wait(lock, [&]() noexcept {
                    return allowRecursiveShutdown;
                });
            }
        }
        if (!firstDiagnostic) {
            return;
        }

        fixture.launcher->shutdown();
        {
            const std::lock_guard lock{gateMutex};
            recursiveShutdownReturned = true;
        }
        gateChanged.notify_all();
    });

    require(fixture.launcher->launch(
                "127.0.0.1", 7788U, fixture.context()).hasValue(),
            "cross-worker helper work was not admitted");
    if (!fixture.supervisor->waitUntilRun(2s)) {
        fixture.supervisor->release();
        fixture.launcher->shutdown();
        fail("cross-worker helper process entry");
    }
    fixture.supervisor->release();

    bool entered{};
    {
        std::unique_lock lock{gateMutex};
        entered = gateChanged.wait_for(
            lock, 2s,
            [&]() noexcept { return launchDiagnosticEntered; });
    }
    const auto conflict = fixture.launcher->launch(
        "127.0.0.1", 7788U, fixture.context());
    const bool secondWorkerAttempted =
        fixture.diagnostics.waitUntilAttempts(2U, 2s);
    {
        const std::lock_guard lock{gateMutex};
        allowRecursiveShutdown = true;
    }
    gateChanged.notify_all();

    bool recursiveReturned{};
    {
        std::unique_lock lock{gateMutex};
        recursiveReturned = gateChanged.wait_for(
            lock, 2s,
            [&]() noexcept { return recursiveShutdownReturned; });
    }
    fixture.launcher->shutdown();
    const auto events = fixture.diagnostics.events();

    require(entered,
            "launch-worker diagnostic did not enter the serialized sink");
    requireError(conflict, Domain::ErrorCodes::Conflict,
                 "cross-worker conflict was admitted");
    require(secondWorkerAttempted,
            "immediate diagnostic worker did not contend for the serialized sink");
    require(recursiveReturned,
            "internal shutdown cross-joined the contending diagnostic worker");
    require(events.size() == 2U,
            "cross-worker shutdown did not drain both exact diagnostics");
    require(WindowsDashboardBrowserLauncherTestAccess::closed(*fixture.launcher),
            "external owner did not exact-join both browser workers");
}

void nativeFailureDiagnosticNeverContainsBearer()
{
    Fixture fixture;
    fixture.supervisor->failWith(Domain::makeError(
        std::string{"malicious\n"} + std::string{Token},
        std::string{"scripted failure must be redacted "} +
            std::string{Token},
        true));
    require(fixture.launcher->launch(
                "127.0.0.1", 7788U, fixture.context()).hasValue(),
            "failing helper work was not admitted");
    if (!fixture.supervisor->waitUntilRun(2s)) {
        fixture.supervisor->release();
        fixture.launcher->shutdown();
        fail("failing dashboard helper process entry");
    }
    fixture.supervisor->release();
    const bool returned = fixture.supervisor->waitUntilReturned(2s);
    fixture.launcher->shutdown();
    const auto events = fixture.diagnostics.events();

    require(returned, "failing dashboard helper did not return");
    require(events.size() == 1U &&
                events.front().event ==
                    "manager_dashboard_browser_open_failed" &&
                events.front().fields.size() == 1U &&
                events.front().fields.front().value ==
                    Domain::ErrorCodes::ProcessLaunchFailed,
            "helper failure diagnostic was not exact");
    for (const auto& event : events) {
        require(event.event.find(Token) == std::string::npos,
                "diagnostic event leaked the bearer");
        for (const auto& field : event.fields) {
            require(field.name.find(Token) == std::string::npos &&
                        field.value.find(Token) == std::string::npos &&
                        field.value.find('\n') == std::string::npos,
                    "diagnostic field leaked the bearer");
        }
    }
}

void deadlineFailureRetainsOnlyTheStaticTypedDiagnostic()
{
    Fixture fixture;
    fixture.supervisor->failWith(Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        std::string{"malicious deadline text\n"} + std::string{Token},
        true));
    require(fixture.launcher->launch(
                "127.0.0.1", 7788U, fixture.context()).hasValue(),
            "deadline helper work was not admitted");
    if (!fixture.supervisor->waitUntilRun(2s)) {
        fixture.supervisor->release();
        fixture.launcher->shutdown();
        fail("deadline dashboard helper process entry");
    }
    fixture.supervisor->release();
    const bool returned = fixture.supervisor->waitUntilReturned(2s);
    fixture.launcher->shutdown();
    const auto events = fixture.diagnostics.events();

    require(returned, "deadline dashboard helper did not return");
    require(events.size() == 1U &&
                events.front().event ==
                    "manager_dashboard_browser_open_failed" &&
                events.front().fields.size() == 1U &&
                events.front().fields.front().value ==
                    Domain::ErrorCodes::DeadlineExceeded,
            "deadline failure lost its static typed diagnostic");
    require(events.front().fields.front().value.find(Token) ==
                    std::string::npos &&
                events.front().fields.front().value.find('\n') ==
                    std::string::npos,
            "deadline diagnostic retained untrusted supervisor text");
}

void stalledWorkHasNonblockingSignalAndExactJoinClosedState()
{
    Fixture fixture;
    fixture.supervisor->ignoreCancellationUntilReleased(true);
    require(fixture.launcher->launch(
                "::1", 65535U, fixture.context()).hasValue(),
            "stalled helper work was not admitted");
    if (!fixture.supervisor->waitUntilRun(2s)) {
        fixture.supervisor->release();
        fixture.launcher->shutdown();
        fail("stalled dashboard helper process entry");
    }

    const auto started = std::chrono::steady_clock::now();
    fixture.launcher->beginShutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto closedAdmission = fixture.launcher->launch(
        "::1", 65535U, fixture.context());
    std::jthread joining{
        [&fixture]() noexcept { fixture.launcher->shutdown(); }};

    // The fake intentionally ignores cancellation until this explicit release;
    // release it before every assertion so a failing assertion cannot strand
    // the exact-join owner.
    fixture.supervisor->release();
    joining.join();
    const auto events = fixture.diagnostics.events();

    require(elapsed < 100ms,
            "beginShutdown blocked on stalled native activation");
    requireError(closedAdmission, Domain::ErrorCodes::TransportClosed,
                 "closed launcher admitted post-shutdown work");
    require(events.size() == 1U &&
                events.front().event ==
                    "manager_dashboard_browser_open_failed" &&
                events.front().fields.size() == 1U &&
                events.front().fields.front().value ==
                    Domain::ErrorCodes::TransportClosed,
            "closed-state rejection was not diagnosed during shutdown");
    require(fixture.supervisor->cancelCalls() >= 1U,
            "shutdown did not cancel the owned helper operation");
    require(fixture.supervisor->activeCalls() == 0U,
            "exact shutdown returned with an active helper operation");
    require(WindowsDashboardBrowserLauncherTestAccess::closed(*fixture.launcher),
            "shutdown race lost the terminal Closed state");
}

void recursiveWorkerShutdownCannotDeadlockExternalJoin()
{
    Fixture fixture;
    fixture.supervisor->release();

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool callbackEntered{};
    bool releaseCallback{};
    bool callbackReturned{};
    fixture.diagnostics.setHook([&]() {
        {
            const std::lock_guard lock{gateMutex};
            callbackEntered = true;
        }
        gateChanged.notify_all();
        {
            std::unique_lock lock{gateMutex};
            gateChanged.wait(lock, [&]() noexcept { return releaseCallback; });
        }
        fixture.launcher->shutdown();
        {
            const std::lock_guard lock{gateMutex};
            callbackReturned = true;
        }
        gateChanged.notify_all();
    });

    require(fixture.launcher->launch(
                "127.0.0.1", 7788U, fixture.context()).hasValue(),
            "recursive-shutdown helper work was not admitted");
    bool entered{};
    {
        std::unique_lock lock{gateMutex};
        entered = gateChanged.wait_for(
            lock, 2s, [&]() noexcept { return callbackEntered; });
    }

    std::jthread externalJoin{
        [&fixture]() noexcept { fixture.launcher->shutdown(); }};
    const bool joinOwned =
        WindowsDashboardBrowserLauncherTestAccess::waitUntilJoinOwned(
            *fixture.launcher, 2s);
    {
        const std::lock_guard lock{gateMutex};
        releaseCallback = true;
    }
    gateChanged.notify_all();
    externalJoin.join();

    require(entered, "diagnostic callback did not run on the helper worker");
    require(joinOwned, "external shutdown did not claim the exact join");
    require(callbackReturned,
            "worker-recursive shutdown deadlocked behind the external join");
    require(WindowsDashboardBrowserLauncherTestAccess::closed(*fixture.launcher),
            "recursive shutdown did not end in Closed state");
}

void helperCommandAcceptsOnlyBoundedCanonicalStdin()
{
    auto ipv4Platform = std::make_unique<RecordingUriPlatform>();
    auto* const ipv4View = ipv4Platform.get();
    auto ipv4 = WindowsDashboardUriActivationCommandTestAccess::create(
        std::move(ipv4Platform));
    std::istringstream ipv4Input{std::string{Ipv4Uri}};
    require(ipv4->run(ipv4Input).hasValue(),
            "canonical IPv4 helper input was rejected");
    require(ipv4View->calls() == 1U && ipv4View->uri() ==
                L"http://127.0.0.1:7788/#token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "canonical IPv4 helper input changed at the Shell boundary");

    auto ipv6Platform = std::make_unique<RecordingUriPlatform>();
    auto* const ipv6View = ipv6Platform.get();
    auto ipv6 = WindowsDashboardUriActivationCommandTestAccess::create(
        std::move(ipv6Platform));
    std::istringstream ipv6Input{std::string{Ipv6Uri}};
    require(ipv6->run(ipv6Input).hasValue(),
            "canonical IPv6 helper input was rejected");
    require(ipv6View->calls() == 1U && ipv6View->uri() ==
                L"http://[::1]:65535/#token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "canonical IPv6 helper input changed at the Shell boundary");

    auto invalidPlatform = std::make_unique<RecordingUriPlatform>();
    auto* const invalidView = invalidPlatform.get();
    auto invalid = WindowsDashboardUriActivationCommandTestAccess::create(
        std::move(invalidPlatform));
    std::istringstream overflow{
        std::string(WindowsDashboardUriActivationCommand::MaximumUriBytes +
                        1U,
                    'x')};
    requireError(invalid->run(overflow), Domain::ErrorCodes::PayloadTooLarge,
                 "exactly 129 helper bytes were not rejected");
    std::istringstream lineTerminated{std::string{Ipv4Uri} + "\r\n"};
    requireError(invalid->run(lineTerminated),
                 Domain::ErrorCodes::InvalidRequest,
                 "CR/LF helper input was accepted");
    std::istringstream external{
        "https://example.com/#token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    requireError(invalid->run(external), Domain::ErrorCodes::InvalidRequest,
                 "non-loopback helper URI was accepted");
    require(invalidView->calls() == 0U,
            "invalid helper input reached the Shell boundary");

    auto failingPlatform = std::make_unique<RecordingUriPlatform>();
    auto* const failingView = failingPlatform.get();
    failingPlatform->failWith(Domain::makeError(
        std::string{"malicious\n"} + std::string{Token},
        std::string{"leaky platform text "} + std::string{Token}, true));
    auto failing = WindowsDashboardUriActivationCommandTestAccess::create(
        std::move(failingPlatform));
    std::istringstream failingInput{std::string{Ipv4Uri}};
    const auto failure = failing->run(failingInput);
    requireError(failure, Domain::ErrorCodes::HostCapabilityUnavailable,
                 "Shell failure code was not normalized");
    require(failure.error().message.find(Token) == std::string::npos &&
                failure.error().message.find(Ipv4Uri) == std::string::npos,
            "Shell error text reflected the URI or bearer");
    require(failingView->calls() == 1U,
            "valid failing helper input did not reach the Shell boundary");
}

} // namespace
} // namespace ForgeConductor::Tests

int wmain(const int argc, wchar_t** argv)
{
    try {
        ForgeConductor::Tests::require(
            argc == 2 && argv != nullptr && argv[1] != nullptr,
            "expected the exact sibling CLI test helper path");
        ForgeConductor::Tests::realHelperRejectsOversizedStdinAtProcessBoundary(
            argv[1]);
        ForgeConductor::Tests::admissionIsPromptAndHelperRequestIsSecretSafe();
        ForgeConductor::Tests::
            endpointContextAndConfigurationRejectBeforeProcessAdmission();
        ForgeConductor::Tests::
            immediateDiagnosticIsNonblockingAndExactlyJoined();
        ForgeConductor::Tests::
            crossWorkerRecursiveShutdownDefersBothExactJoins();
        ForgeConductor::Tests::nativeFailureDiagnosticNeverContainsBearer();
        ForgeConductor::Tests::
            deadlineFailureRetainsOnlyTheStaticTypedDiagnostic();
        ForgeConductor::Tests::
            stalledWorkHasNonblockingSignalAndExactJoinClosedState();
        ForgeConductor::Tests::
            recursiveWorkerShutdownCannotDeadlockExternalJoin();
        ForgeConductor::Tests::helperCommandAcceptsOnlyBoundedCanonicalStdin();
        std::cout << "Windows dashboard browser launcher tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows dashboard browser launcher tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Windows dashboard browser launcher tests failed with an unknown error.\n";
        return 1;
    }
}
