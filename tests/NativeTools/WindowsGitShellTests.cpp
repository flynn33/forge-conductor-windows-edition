#include "../Infrastructure/TestSupport.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace ForgeConductor;
using namespace ForgeConductor::Tests;
using NativeTools::Windows::WindowsGitService;
using NativeTools::Windows::WindowsShellService;
using namespace std::chrono_literals;

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationId operationId(const std::uint32_t sequence)
{
    auto text = std::string{"10000000-0000-4000-8000-"};
    auto suffix = std::to_string(sequence);
    text.append(12U - suffix.size(), '0');
    text.append(suffix);
    return parse<Domain::OperationId>(text);
}

[[nodiscard]] Domain::OperationContext context(const std::uint32_t sequence)
{
    return Domain::OperationContext{
        operationId(sequence),
        std::chrono::steady_clock::now() + 5min,
        {},
        parse<Domain::CorrelationId>("p13-git-shell-tests")};
}

class ScriptedProcessSupervisor final : public Contracts::IProcessSupervisor {
public:
    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            requests_.push_back(request);
            authorityIds_.push_back(authority.authorityId());
            projectIds_.push_back(authority.projectId());
            operationIds_.push_back(operationContext.operationId);
            if (outcomes_.empty()) {
                return Domain::Result<Domain::ProcessResult>::success(
                    Domain::ProcessResult{});
            }
            auto result = std::move(outcomes_.front());
            outcomes_.pop_front();
            return result;
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted process request could not be recorded."));
        }
    }

    void cancel(const Domain::OperationId& id) noexcept override
    {
        try {
            cancelled_.push_back(id);
        } catch (...) {
        }
    }

    void cancelAll() noexcept override { ++cancelAllCalls_; }
    void shutdown() noexcept override { ++shutdownCalls_; }

    void enqueue(Domain::ProcessResult result)
    {
        outcomes_.push_back(
            Domain::Result<Domain::ProcessResult>::success(std::move(result)));
    }

    void enqueue(Domain::Error error)
    {
        outcomes_.push_back(
            Domain::Result<Domain::ProcessResult>::failure(std::move(error)));
    }

    [[nodiscard]] const std::vector<Domain::ProcessRequest>& requests() const noexcept
    {
        return requests_;
    }

    [[nodiscard]] const std::vector<Domain::AuthorityId>& authorityIds() const noexcept
    {
        return authorityIds_;
    }

    [[nodiscard]] const std::vector<Domain::ProjectId>& projectIds() const noexcept
    {
        return projectIds_;
    }

    [[nodiscard]] const std::vector<Domain::OperationId>& operationIds() const noexcept
    {
        return operationIds_;
    }

    [[nodiscard]] const std::vector<Domain::OperationId>& cancelled() const noexcept
    {
        return cancelled_;
    }

    [[nodiscard]] std::size_t cancelAllCalls() const noexcept
    {
        return cancelAllCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        return shutdownCalls_;
    }

private:
    std::deque<Domain::Result<Domain::ProcessResult>> outcomes_;
    std::vector<Domain::ProcessRequest> requests_;
    std::vector<Domain::AuthorityId> authorityIds_;
    std::vector<Domain::ProjectId> projectIds_;
    std::vector<Domain::OperationId> operationIds_;
    std::vector<Domain::OperationId> cancelled_;
    std::size_t cancelAllCalls_{};
    std::size_t shutdownCalls_{};
};

class AdmissionBarrierProcessSupervisor final
    : public Contracts::IProcessSupervisor {
public:
    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest&,
        const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            bool cancellationObserved{};
            {
                std::unique_lock lock{mutex_};
                cancellation_ = operationContext.cancellation;
                runEntered_ = true;
                enteredCondition_.notify_all();
                releaseCondition_.wait(lock, [this] { return runReleased_; });
                cancellationObserved = cancellation_.stop_requested();
                if (!cancellationObserved) {
                    ++launches_;
                }
            }
            if (cancellationObserved) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The admission-barrier process was cancelled before launch."));
            }
            return Domain::Result<Domain::ProcessResult>::success(
                Domain::ProcessResult{});
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The admission-barrier process supervisor failed."));
        }
    }

    void cancel(const Domain::OperationId&) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            ++cancelCalls_;
        } catch (...) {
        }
    }

    void cancelAll() noexcept override {}
    void shutdown() noexcept override {}

    [[nodiscard]] bool waitUntilRunEntered(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return enteredCondition_.wait_for(
                lock, timeout, [this] { return runEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releaseRun() noexcept
    {
        try {
            {
                std::scoped_lock lock{mutex_};
                runReleased_ = true;
            }
            releaseCondition_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool cancellationTokenStopped() const noexcept
    {
        try {
            std::scoped_lock lock{mutex_};
            return cancellation_.stop_requested();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        try {
            std::scoped_lock lock{mutex_};
            return cancelCalls_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t launches() const noexcept
    {
        try {
            std::scoped_lock lock{mutex_};
            return launches_;
        } catch (...) {
            return 0U;
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable enteredCondition_;
    std::condition_variable releaseCondition_;
    std::stop_token cancellation_;
    bool runEntered_{};
    bool runReleased_{};
    std::size_t cancelCalls_{};
    std::size_t launches_{};
};

struct AuthorityFixture final {
    Domain::MonotonicTimePoint now{};
    Domain::PathText workspaceRoot{path("C:\\workspace\\project")};
    Domain::PathText toolsRoot{path("C:\\tools")};
    Domain::PathText gitExecutable{path("C:\\tools\\git.exe")};
    Domain::PathText powerShellExecutable{path("C:\\tools\\pwsh.exe")};
    Domain::ProjectId projectId{parse<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000001")};
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000001"),
        parse<Domain::ClientId>("p13-client"),
        {workspaceRoot, toolsRoot},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        1U};
    Contracts::WorkspaceAuthority authority;
    Contracts::AuthorizedPath readRepository;
    Contracts::AuthorizedPath writeRepository;
    Contracts::AuthorizedPath readFile;

    AuthorityFixture()
        : authority{take(issuer.authorityFor(projectId, context(1U)))},
          readRepository{take(issuer.authorize(
              authority,
              Domain::PathAuthorizationRequest{
                  workspaceRoot,
                  std::optional<Domain::PathText>{workspaceRoot},
                  Domain::FileAccess::Read,
                  false},
              context(2U)))},
          writeRepository{take(issuer.authorize(
              authority,
              Domain::PathAuthorizationRequest{
                  workspaceRoot,
                  std::optional<Domain::PathText>{workspaceRoot},
                  Domain::FileAccess::Write,
                  false},
              context(3U)))},
          readFile{take(issuer.authorize(
              authority,
              Domain::PathAuthorizationRequest{
                  path("C:\\workspace\\project\\src\\main.cpp"),
                  std::optional<Domain::PathText>{workspaceRoot},
                  Domain::FileAccess::Read,
                  false},
              context(4U)))}
    {
        issuer.setNow(now);
    }
};

[[nodiscard]] Domain::ProcessResult processResult(
    const std::int32_t exitCode = 0,
    std::string standardOutput = {},
    std::string standardError = {})
{
    Domain::ProcessResult result;
    result.exitCode = exitCode;
    result.stdoutUtf8 = std::move(standardOutput);
    result.stderrUtf8 = std::move(standardError);
    return result;
}

[[nodiscard]] Domain::ProcessRequest shellRequest(
    const AuthorityFixture& fixture,
    std::string command)
{
    Domain::ProcessRequest request{fixture.powerShellExecutable};
    request.arguments.push_back(std::move(command));
    request.workingDirectory = fixture.workspaceRoot;
    return request;
}

void gitCommandsUseExactDirectArgv()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsGitService git{
        fixture.gitExecutable, fixture.authority, supervisor};

    supervisor->enqueue(processResult(0, "## main\n"));
    require(
        take(git.status(fixture.readRepository, 1'024U, context(10U))).stdoutUtf8 ==
            "## main\n",
        "Git status did not return stdout");
    supervisor->enqueue(processResult(0, "diff\n"));
    const std::vector<std::string> staged{"--cached"};
    require(
        take(git.diff(
            fixture.readRepository, staged, 2'048U, context(11U))).stdoutUtf8 ==
            "diff\n",
        "Git staged diff did not return stdout");
    supervisor->enqueue(processResult(0, "abc title\n"));
    require(
        take(git.log(fixture.readRepository, 20U, 4'096U, context(12U))).stdoutUtf8 ==
            "abc title\n",
        "Git log did not return stdout");
    supervisor->enqueue(processResult());
    require(
        git.add(fixture.writeRepository, {}, context(13U)).hasValue(),
        "Git add -A failed");
    supervisor->enqueue(processResult());
    const std::vector<Contracts::AuthorizedPath> paths{fixture.readFile};
    require(
        git.add(fixture.writeRepository, paths, context(14U)).hasValue(),
        "Git add path failed");
    supervisor->enqueue(processResult(0, "committed\n"));
    require(
        take(git.commit(fixture.writeRepository, {}, context(15U))).stdoutUtf8 ==
            "committed\n",
        "Git default commit failed");

    const auto& requests = supervisor->requests();
    require(requests.size() == 6U, "Git did not make exactly six process calls");
    require(
        requests[0].executable == fixture.gitExecutable &&
            requests[0].arguments ==
                std::vector<std::string>{"status", "--porcelain=v1", "-b"},
        "Git status argv did not match macOS behavior");
    require(
        requests[1].arguments ==
            std::vector<std::string>{"diff", "--cached"},
        "Git diff argv did not match macOS behavior");
    require(
        requests[2].arguments ==
            std::vector<std::string>{"log", "-n", "20", "--oneline"},
        "Git log argv did not match macOS behavior");
    require(
        requests[3].arguments == std::vector<std::string>{"add", "-A"},
        "Git add -A argv did not match macOS behavior");
    require(
        requests[4].arguments == std::vector<std::string>{
            "add", "--", "C:\\workspace\\project\\src\\main.cpp"},
        "Git add path was not passed as a direct argv element");
    require(
        requests[5].arguments == std::vector<std::string>{
            "commit", "-m", "chore: forge-conductor commit"},
        "Git default commit argv did not match macOS behavior");
    for (const auto& request : requests) {
        require(
            request.workingDirectory == fixture.workspaceRoot &&
                request.timeout == 30s && request.inheritEnvironment &&
                request.maximumStderrBytes == 20'000U &&
                request.environment.size() == 1U &&
                request.environment.front().name == "GIT_TERMINAL_PROMPT" &&
                request.environment.front().value == "0",
            "Git process policy changed across commands");
    }
    require(
        requests[0].maximumStdoutBytes == 1'024U &&
            requests[1].maximumStdoutBytes == 2'048U &&
            requests[2].maximumStdoutBytes == 4'096U &&
            requests[3].maximumStdoutBytes == 80'000U,
        "Git stdout bounds were not propagated exactly");
    require(
        supervisor->authorityIds().front() == fixture.authority.authorityId() &&
            supervisor->projectIds().front() == fixture.projectId &&
            supervisor->operationIds().front() == operationId(10U),
        "Git did not preserve authority, project, and operation binding");
}

void gitBoundsAndAuthorityFailBeforeProcessDispatch()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsGitService git{
        fixture.gitExecutable, fixture.authority, supervisor};

    requireError(
        git.status(fixture.readRepository, 80'001U, context(20U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git accepted an excessive output bound");
    requireError(
        git.log(fixture.readRepository, 201U, 1'024U, context(21U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git accepted more than 200 log entries");
    const std::vector<std::string> unsupported{"--stat"};
    requireError(
        git.diff(fixture.readRepository, unsupported, 1'024U, context(22U)),
        Domain::ErrorCodes::InvalidRequest,
        "Git diff accepted behavior outside the macOS staged option");
    requireError(
        git.add(fixture.readRepository, {}, context(23U)),
        Domain::ErrorCodes::Unauthorized,
        "Git add accepted a read-only repository capability");
    const std::vector<Contracts::AuthorizedPath> tooManyPaths(
        WindowsGitService::MaximumAddPaths + 1U, fixture.readFile);
    requireError(
        git.add(fixture.writeRepository, tooManyPaths, context(24U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git add accepted more than 200 paths");
    requireError(
        git.commit(
            fixture.writeRepository,
            std::string(WindowsGitService::MaximumArgumentBytes + 1U, 'x'),
            context(25U)),
        Domain::ErrorCodes::PayloadTooLarge,
        "Git commit accepted an oversized message");

    Fakes::DeterministicWorkspaceAuthority otherIssuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000002"),
        parse<Domain::ClientId>("p13-client"),
        {fixture.workspaceRoot, fixture.toolsRoot},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read, Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        1U};
    const auto otherAuthority = take(
        otherIssuer.authorityFor(fixture.projectId, context(26U)));
    const auto otherRepository = take(otherIssuer.authorize(
        otherAuthority,
        Domain::PathAuthorizationRequest{
            fixture.workspaceRoot,
            std::optional<Domain::PathText>{fixture.workspaceRoot},
            Domain::FileAccess::Read,
            false},
        context(27U)));
    requireError(
        git.status(otherRepository, 1'024U, context(28U)),
        Domain::ErrorCodes::Unauthorized,
        "Git accepted a repository with another authority identifier");

    WindowsGitService relativeExecutable{
        path("git.exe"), fixture.authority, supervisor};
    requireError(
        relativeExecutable.status(
            fixture.readRepository, 1'024U, context(28U)),
        Domain::ErrorCodes::InvalidRequest,
        "Git accepted ambient executable lookup");
    require(
        supervisor->requests().empty(),
        "A rejected Git request reached the process supervisor");
}

void gitProcessOutcomesRemainStructuredAndBounded()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsGitService git{
        fixture.gitExecutable, fixture.authority, supervisor};

    supervisor->enqueue(processResult(2, "partial", "fatal"));
    const auto nonzero = take(
        git.status(fixture.readRepository, 1'024U, context(30U)));
    require(
        nonzero.exitCode == 2 && nonzero.stdoutUtf8 == "partial" &&
            nonzero.stderrUtf8 == "fatal",
        "Git discarded a nonzero process result required by the MCP payload");
    auto timeout = processResult(1);
    timeout.timedOut = true;
    supervisor->enqueue(std::move(timeout));
    require(
        take(git.status(fixture.readRepository, 1'024U, context(31U))).timedOut,
        "Git discarded a process timeout flag");
    auto cancelled = processResult(1);
    cancelled.cancelled = true;
    supervisor->enqueue(std::move(cancelled));
    require(
        take(git.status(fixture.readRepository, 1'024U, context(32U))).cancelled,
        "Git discarded a process cancellation flag");
    auto truncated = processResult(0, std::string(1'025U, 'p'));
    supervisor->enqueue(std::move(truncated));
    const auto bounded = take(
        git.status(fixture.readRepository, 1'024U, context(33U)));
    require(
        bounded.stdoutTruncated && bounded.stdoutUtf8.size() == 1'024U,
        "Git did not preserve a bounded structured process result");
    supervisor->enqueue(Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "scripted deadline"));
    requireError(
        git.status(fixture.readRepository, 1'024U, context(34U)),
        Domain::ErrorCodes::DeadlineExceeded,
        "Git did not preserve the process supervisor error");
}

void shellUsesFixedPowerShellAndClampedBudgets()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsShellService shell{
        fixture.powerShellExecutable, fixture.authority, supervisor};
    Fakes::DeterministicWorkspaceAuthority callerIssuer{
        fixture.authority.authorityId(),
        fixture.authority.callerId(),
        {fixture.workspaceRoot},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        fixture.authority.generation()};
    const auto callerAuthority = take(
        callerIssuer.authorityFor(fixture.projectId, context(39U)));

    auto zeroTimeout = shellRequest(fixture, "Get-Location");
    zeroTimeout.timeout = 0ms;
    requireError(
        shell.execute(zeroTimeout, callerAuthority, context(38U)),
        Domain::ErrorCodes::InvalidRequest,
        "Shell accepted a zero timeout");
    auto negativeTimeout = shellRequest(fixture, "Get-Location");
    negativeTimeout.timeout = -1ms;
    requireError(
        shell.execute(negativeTimeout, callerAuthority, context(37U)),
        Domain::ErrorCodes::InvalidRequest,
        "Shell accepted a negative timeout");
    require(supervisor->requests().empty(),
            "Invalid shell timeouts reached the process supervisor");

    auto result = processResult(7, std::string(80'001U, 'o'), std::string(20'001U, 'e'));
    result.timedOut = true;
    result.cancelled = true;
    supervisor->enqueue(std::move(result));
    auto request = shellRequest(fixture, "Write-Output 'ok'");
    request.maximumStdoutBytes = 100'000U;
    request.maximumStderrBytes = 30'000U;
    request.environment.push_back({"FORGE_TEST", "one"});
    request.inheritEnvironment = true;
    const auto response = take(shell.execute(
        request, callerAuthority, context(40U)));

    require(
        response.exitCode == 7 && response.timedOut && response.cancelled &&
            response.stdoutTruncated && response.stderrTruncated &&
            response.stdoutUtf8.size() == 80'000U &&
            response.stderrUtf8.size() == 20'000U,
        "Shell did not preserve status flags or enforce output bounds");
    require(
        supervisor->requests().size() == 1U,
        "Shell did not make exactly one process call");
    const auto& normalized = supervisor->requests().front();
    require(
        normalized.executable == fixture.powerShellExecutable &&
            normalized.arguments == std::vector<std::string>{
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Write-Output 'ok'"},
        "Shell did not own the exact PowerShell argv");
    require(
        normalized.workingDirectory == fixture.workspaceRoot &&
            normalized.timeout == 30s &&
            normalized.maximumStdoutBytes == 80'000U &&
            normalized.maximumStderrBytes == 20'000U &&
            normalized.environment.size() == 1U &&
            normalized.environment.front().name == "FORGE_TEST" &&
            normalized.environment.front().value == "one" &&
            normalized.inheritEnvironment,
        "Shell did not preserve the authorized envelope and clamp its budgets");
    require(
        supervisor->authorityIds().front() == fixture.authority.authorityId() &&
            supervisor->projectIds().front() == fixture.projectId,
        "Shell did not keep its private execution authority bound to the caller identity");

    supervisor->enqueue(processResult());
    auto longTimeout = shellRequest(fixture, "Get-Location");
    longTimeout.timeout = 10min;
    require(
        shell.execute(longTimeout, callerAuthority, context(41U)).hasValue(),
        "Shell rejected a timeout that should be clamped");
    require(
        supervisor->requests().back().timeout == 120s,
        "Shell did not clamp timeout to 120 seconds");

    auto multibyte = processResult(0, "\xE2\x82\xACx");
    supervisor->enqueue(std::move(multibyte));
    auto splitBoundary = shellRequest(fixture, "Get-Location");
    splitBoundary.maximumStdoutBytes = 2U;
    const auto safelyTruncated = take(shell.execute(
        splitBoundary, callerAuthority, context(42U)));
    require(
        safelyTruncated.stdoutTruncated &&
            safelyTruncated.stdoutUtf8.empty() &&
            Domain::isValidUtf8(safelyTruncated.stdoutUtf8),
        "Shell split a UTF-8 scalar while enforcing its decoded output cap");
}

void shellPolicyAuthorityCancellationAndShutdownFailClosed()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsShellService shell{
        fixture.powerShellExecutable, fixture.authority, supervisor};

    auto wrongExecutable = shellRequest(fixture, "Get-Location");
    wrongExecutable.executable = fixture.gitExecutable;
    requireError(
        shell.execute(wrongExecutable, fixture.authority, context(50U)),
        Domain::ErrorCodes::InvalidRequest,
        "Shell accepted a different executable");
    auto missingCommand = shellRequest(fixture, "Get-Location");
    missingCommand.arguments.clear();
    requireError(
        shell.execute(missingCommand, fixture.authority, context(51U)),
        Domain::ErrorCodes::InvalidRequest,
        "Shell accepted a missing command");
    auto oversized = shellRequest(
        fixture, std::string(WindowsShellService::MaximumCommandBytes + 1U, 'x'));
    requireError(
        shell.execute(oversized, fixture.authority, context(52U)),
        Domain::ErrorCodes::PayloadTooLarge,
        "Shell accepted an oversized command");
    auto outside = shellRequest(fixture, "Get-Location");
    outside.workingDirectory = path("C:\\outside");
    requireError(
        shell.execute(outside, fixture.authority, context(53U)),
        Domain::ErrorCodes::PathOutsideAuthority,
        "Shell accepted an unauthorized working directory");

    const auto otherProject = parse<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000002");
    const auto wrongProjectAuthority = take(
        fixture.issuer.authorityFor(otherProject, context(54U)));
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            wrongProjectAuthority,
            context(55U)),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "Shell accepted authority from another project");

    Fakes::DeterministicWorkspaceAuthority otherAuthorityIssuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000004"),
        parse<Domain::ClientId>("p13-client"),
        {fixture.workspaceRoot, fixture.toolsRoot},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        1U};
    const auto otherAuthority = take(
        otherAuthorityIssuer.authorityFor(fixture.projectId, context(56U)));
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            otherAuthority,
            context(57U)),
        Domain::ErrorCodes::Unauthorized,
        "Shell accepted a different authority identifier");

    Fakes::DeterministicWorkspaceAuthority disabledIssuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000003"),
        parse<Domain::ClientId>("p13-client"),
        {fixture.workspaceRoot, fixture.toolsRoot},
        Domain::FileAccess::Execute,
        {Domain::FileAccess::Read, Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        false,
        1U};
    const auto disabledAuthority = take(
        disabledIssuer.authorityFor(fixture.projectId, context(58U)));
    WindowsShellService disabledShell{
        fixture.powerShellExecutable, disabledAuthority, supervisor};
    requireError(
        disabledShell.execute(
            shellRequest(fixture, "Get-Location"),
            disabledAuthority,
            context(59U)),
        Domain::ErrorCodes::ShellDisabled,
        "Shell ignored the disabled-by-default authority policy");

    const auto cancelledId = operationId(60U);
    shell.cancel(cancelledId);
    require(
        supervisor->cancelled().empty(),
        "Shell forwarded cancellation for an operation it does not own");
    supervisor->enqueue(Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        "scripted cancellation"));
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            fixture.authority,
            context(61U)),
        Domain::ErrorCodes::Cancelled,
        "Shell did not preserve a supervisor cancellation error");

    const auto callsBeforeShutdown = supervisor->requests().size();
    shell.shutdown();
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            fixture.authority,
            context(62U)),
        Domain::ErrorCodes::Cancelled,
        "Shell accepted work after shutdown");
    require(
        supervisor->requests().size() == callsBeforeShutdown &&
            supervisor->cancelAllCalls() == 0U &&
            supervisor->shutdownCalls() == 0U,
        "Shell shutdown affected the shared supervisor or dispatched new work");
}

void shellShutdownCancelsBeforeSupervisorAdmission()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<AdmissionBarrierProcessSupervisor>();
    auto shell = std::make_unique<WindowsShellService>(
        fixture.powerShellExecutable, fixture.authority, supervisor);
    auto* const shellView = shell.get();
    std::optional<Domain::Result<Domain::ProcessResult>> outcome;
    std::jthread execution{[&] {
        outcome.emplace(shellView->execute(
            shellRequest(fixture, "Write-Output 'must-not-launch'"),
            fixture.authority,
            context(63U)));
    }};

    const auto entered = supervisor->waitUntilRunEntered(5s);
    if (entered) {
        shell.reset();
    } else {
        shell->shutdown();
    }
    const auto derivedTokenStopped = supervisor->cancellationTokenStopped();
    const auto forwardedCancels = supervisor->cancelCalls();
    supervisor->releaseRun();
    execution.join();
    shell.reset();

    require(
        entered,
        "Shell admission-race fixture did not reach the supervisor barrier");
    require(
        derivedTokenStopped,
        "Shell destruction did not persist cancellation across supervisor admission");
    require(
        forwardedCancels == 1U,
        "Shell destruction did not forward cancellation after stopping locally");
    require(
        outcome.has_value() && !outcome.value() &&
            outcome->error().code == Domain::ErrorCodes::Cancelled,
        "Shell admission race returned success after destruction");
    require(
        supervisor->launches() == 0U,
        "Shell admission race reached launch after destruction");
}

} // namespace

int main()
{
    static_assert(std::is_final_v<WindowsGitService>);
    static_assert(std::is_final_v<WindowsShellService>);
    static_assert(!std::is_copy_constructible_v<WindowsGitService>);
    static_assert(!std::is_copy_constructible_v<WindowsShellService>);

    try {
        gitCommandsUseExactDirectArgv();
        std::cout << "PASS native_tools.git_direct_argv\n";
        gitBoundsAndAuthorityFailBeforeProcessDispatch();
        std::cout << "PASS native_tools.git_bounds_authority\n";
        gitProcessOutcomesRemainStructuredAndBounded();
        std::cout << "PASS native_tools.git_process_outcomes\n";
        shellUsesFixedPowerShellAndClampedBudgets();
        std::cout << "PASS native_tools.shell_fixed_powershell\n";
        shellPolicyAuthorityCancellationAndShutdownFailClosed();
        std::cout << "PASS native_tools.shell_policy_shutdown\n";
        shellShutdownCancelsBeforeSupervisorAdmission();
        std::cout << "PASS native_tools.shell_admission_shutdown_race\n";
        std::cout << "SUMMARY passed=6 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
