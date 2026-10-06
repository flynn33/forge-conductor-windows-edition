#include "../Infrastructure/TestSupport.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
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

[[nodiscard]] bool containsRoot(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& root) noexcept
{
    return std::find(
               authority.trustedRoots().begin(),
               authority.trustedRoots().end(),
               root) != authority.trustedRoots().end();
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
            authorities_.push_back(authority);
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

    [[nodiscard]] const std::vector<Contracts::WorkspaceAuthority>&
    authorities() const noexcept
    {
        return authorities_;
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
    std::vector<Contracts::WorkspaceAuthority> authorities_;
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

class JobProcessSupervisor final : public Contracts::IProcessSupervisor {
public:
    enum class Mode { Complete, Nonzero, Timeout, Error, Block };
    explicit JobProcessSupervisor(const Mode mode) : mode_{mode} {}

    Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request, const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext& operationContext) noexcept override
    {
        std::unique_lock lock{mutex_};
        requests_.push_back(request);
        operationIds_.push_back(operationContext.operationId);
        deadlines_.push_back(operationContext.deadline);
        started_.notify_all();
        if (mode_ == Mode::Block) {
            released_.wait(lock, operationContext.cancellation, [this] { return release_; });
        }
        if (mode_ == Mode::Error) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::ProcessLaunchFailed, "job launch failed"));
        }
        Domain::ProcessResult result;
        result.stdoutUtf8 = std::string(90'000U, 'o');
        result.stderrUtf8 = "job stderr";
        result.cancelled = operationContext.cancellation.stop_requested();
        result.timedOut = mode_ == Mode::Timeout;
        result.exitCode = mode_ == Mode::Nonzero ? 17 : 0;
        return Domain::Result<Domain::ProcessResult>::success(std::move(result));
    }
    void cancel(const Domain::OperationId&) noexcept override {}
    void cancelAll() noexcept override {}
    void shutdown() noexcept override {}

    bool waitUntilStarted(const std::size_t count)
    {
        std::unique_lock lock{mutex_};
        return started_.wait_for(lock, 3s, [this, count] { return requests_.size() >= count; });
    }
    Domain::ProcessRequest lastRequest()
    {
        std::scoped_lock lock{mutex_};
        return requests_.back();
    }
    Domain::MonotonicTimePoint lastDeadline()
    {
        std::scoped_lock lock{mutex_};
        return deadlines_.back();
    }
    Domain::OperationId lastOperation()
    {
        std::scoped_lock lock{mutex_};
        return operationIds_.back();
    }
private:
    const Mode mode_;
    std::mutex mutex_;
    std::condition_variable started_;
    std::condition_variable_any released_;
    std::vector<Domain::ProcessRequest> requests_;
    std::vector<Domain::OperationId> operationIds_;
    std::vector<Domain::MonotonicTimePoint> deadlines_;
    bool release_{};
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
        {workspaceRoot},
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
        fixture.gitExecutable, supervisor};

    supervisor->enqueue(processResult(0, "## main\n"));
    require(
        take(git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(10U))).stdoutUtf8 ==
            "## main\n",
        "Git status did not return stdout");
    supervisor->enqueue(processResult(0, "diff\n"));
    const std::vector<std::string> staged{"--cached"};
    require(
        take(git.diff(
            fixture.readRepository, fixture.authority, staged, 2'048U,
            context(11U))).stdoutUtf8 ==
            "diff\n",
        "Git staged diff did not return stdout");
    supervisor->enqueue(processResult(0, "abc title\n"));
    require(
        take(git.log(
            fixture.readRepository, fixture.authority, 20U, 4'096U,
            context(12U))).stdoutUtf8 ==
            "abc title\n",
        "Git log did not return stdout");
    supervisor->enqueue(processResult());
    require(
        git.add(
            fixture.writeRepository, fixture.authority, {},
            context(13U)).hasValue(),
        "Git add -A failed");
    supervisor->enqueue(processResult());
    const std::vector<Contracts::AuthorizedPath> paths{fixture.readFile};
    require(
        git.add(
            fixture.writeRepository, fixture.authority, paths,
            context(14U)).hasValue(),
        "Git add path failed");
    supervisor->enqueue(processResult(0, "committed\n"));
    require(
        take(git.commit(
            fixture.writeRepository, fixture.authority, {},
            context(15U))).stdoutUtf8 ==
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
    for (const auto& authority : supervisor->authorities()) {
        require(
            authority.authorityId() == fixture.authority.authorityId() &&
                authority.projectId() == fixture.authority.projectId() &&
                authority.callerId() == fixture.authority.callerId() &&
                authority.generation() == fixture.authority.generation() &&
                authority.shellEnabled() &&
                authority.grants() ==
                    std::vector<Domain::FileAccess>{Domain::FileAccess::Execute} &&
                containsRoot(authority, fixture.workspaceRoot) &&
                containsRoot(authority, fixture.toolsRoot),
            "Git did not derive the expected private per-call execution authority");
    }
    require(
        !containsRoot(fixture.authority, fixture.toolsRoot),
        "Git exposed its executable parent through caller authority");
}

void gitBoundsAndAuthorityFailBeforeProcessDispatch()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsGitService git{
        fixture.gitExecutable, supervisor};

    requireError(
        git.status(
            fixture.readRepository, fixture.authority, 80'001U,
            context(20U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git accepted an excessive output bound");
    requireError(
        git.log(
            fixture.readRepository, fixture.authority, 201U, 1'024U,
            context(21U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git accepted more than 200 log entries");
    const std::vector<std::string> unsupported{"--stat"};
    requireError(
        git.diff(
            fixture.readRepository, fixture.authority, unsupported, 1'024U,
            context(22U)),
        Domain::ErrorCodes::InvalidRequest,
        "Git diff accepted behavior outside the macOS staged option");
    requireError(
        git.add(
            fixture.readRepository, fixture.authority, {}, context(23U)),
        Domain::ErrorCodes::Unauthorized,
        "Git add accepted a read-only repository capability");
    const std::vector<Contracts::AuthorizedPath> tooManyPaths(
        WindowsGitService::MaximumAddPaths + 1U, fixture.readFile);
    requireError(
        git.add(
            fixture.writeRepository, fixture.authority, tooManyPaths,
            context(24U)),
        Domain::ErrorCodes::LimitExceeded,
        "Git add accepted more than 200 paths");
    requireError(
        git.commit(
            fixture.writeRepository,
            fixture.authority,
            std::string(WindowsGitService::MaximumArgumentBytes + 1U, 'x'),
            context(25U)),
        Domain::ErrorCodes::PayloadTooLarge,
        "Git commit accepted an oversized message");

    Fakes::DeterministicWorkspaceAuthority otherIssuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000002"),
        parse<Domain::ClientId>("p13-client"),
        {fixture.workspaceRoot},
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
        git.status(
            otherRepository, fixture.authority, 1'024U, context(28U)),
        Domain::ErrorCodes::Unauthorized,
        "Git accepted a repository with another authority identifier");

    WindowsGitService relativeExecutable{
        path("git.exe"), supervisor};
    requireError(
        relativeExecutable.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(28U)),
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
        fixture.gitExecutable, supervisor};

    supervisor->enqueue(processResult(2, "partial", "fatal"));
    const auto nonzero = take(
        git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(30U)));
    require(
        nonzero.exitCode == 2 && nonzero.stdoutUtf8 == "partial" &&
            nonzero.stderrUtf8 == "fatal",
        "Git discarded a nonzero process result required by the MCP payload");
    auto timeout = processResult(1);
    timeout.timedOut = true;
    supervisor->enqueue(std::move(timeout));
    require(
        take(git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(31U))).timedOut,
        "Git discarded a process timeout flag");
    auto cancelled = processResult(1);
    cancelled.cancelled = true;
    supervisor->enqueue(std::move(cancelled));
    require(
        take(git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(32U))).cancelled,
        "Git discarded a process cancellation flag");
    auto truncated = processResult(0, std::string(1'025U, 'p'));
    supervisor->enqueue(std::move(truncated));
    const auto bounded = take(
        git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(33U)));
    require(
        bounded.stdoutTruncated && bounded.stdoutUtf8.size() == 1'024U,
        "Git did not preserve a bounded structured process result");
    supervisor->enqueue(Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "scripted deadline"));
    requireError(
        git.status(
            fixture.readRepository, fixture.authority, 1'024U,
            context(34U)),
        Domain::ErrorCodes::DeadlineExceeded,
        "Git did not preserve the process supervisor error");
}

void shellUsesFixedPowerShellAndClampedBudgets()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsShellService shell{
        fixture.powerShellExecutable, supervisor};
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
    request.inheritEnvironment = false;
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
    const auto normalized = supervisor->requests().front();
    require(
        normalized.executable == fixture.powerShellExecutable &&
            normalized.arguments == std::vector<std::string>{
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
                "$OutputEncoding=[Console]::OutputEncoding;"
                "[Console]::InputEncoding=[System.Text.UTF8Encoding]::new($false);"
                "& ([scriptblock]::Create([Console]::In.ReadToEnd()))"} &&
            normalized.stdinUtf8 == "Write-Output 'ok'\n"
                "if (!$?) {exit 1}",
        "Shell did not own the exact PowerShell argv");
    const auto environmentValue = [](
                                      const std::vector<Domain::EnvironmentVariable>& environment,
                                      const std::string_view name) -> const std::string* {
        for (const auto& variable : environment) {
            if (variable.name.size() != name.size()) {
                continue;
            }
            bool matches = true;
            for (std::size_t index = 0U; index < name.size(); ++index) {
                unsigned char left = static_cast<unsigned char>(variable.name[index]);
                unsigned char right = static_cast<unsigned char>(name[index]);
                if (left >= 'A' && left <= 'Z') {
                    left = static_cast<unsigned char>(left - 'A' + 'a');
                }
                if (right >= 'A' && right <= 'Z') {
                    right = static_cast<unsigned char>(right - 'A' + 'a');
                }
                if (left != right) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return &variable.value;
            }
        }
        return nullptr;
    };
    const auto containsAscii = [](const std::string_view value, const std::string_view needle) {
        if (needle.empty() || needle.size() > value.size()) {
            return false;
        }
        for (std::size_t start = 0U; start <= value.size() - needle.size(); ++start) {
            bool matches = true;
            for (std::size_t index = 0U; index < needle.size(); ++index) {
                unsigned char left = static_cast<unsigned char>(value[start + index]);
                unsigned char right = static_cast<unsigned char>(needle[index]);
                if (left >= 'A' && left <= 'Z') {
                    left = static_cast<unsigned char>(left - 'A' + 'a');
                }
                if (right >= 'A' && right <= 'Z') {
                    right = static_cast<unsigned char>(right - 'A' + 'a');
                }
                if (left != right) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
        }
        return false;
    };
    const auto* forgeTest = environmentValue(normalized.environment, "FORGE_TEST");
    const auto* path = environmentValue(normalized.environment, "PATH");
    const auto* pathExt = environmentValue(normalized.environment, "PATHEXT");
    const auto* comSpec = environmentValue(normalized.environment, "COMSPEC");
    for (const auto& variable : normalized.environment) {
        const auto nul = variable.value.find('\0');
        require(
            !variable.name.empty() &&
                variable.name.find('=') == std::string::npos &&
                variable.name.find('\0') == std::string::npos &&
                nul == std::string::npos,
            "Shell produced an invalid explicit environment entry: " + variable.name +
                " value_bytes=" + std::to_string(variable.value.size()) +
                " nul_at=" + std::to_string(nul));
    }
    require(
        normalized.workingDirectory == fixture.workspaceRoot &&
            normalized.timeout == 30s &&
            normalized.maximumStdoutBytes == 80'000U &&
            normalized.maximumStderrBytes == 20'000U &&
            normalized.environment.size() >= 6U &&
            normalized.environment.size() <= 13U &&
            environmentValue(normalized.environment, "PYTHONUTF8") != nullptr &&
            *environmentValue(normalized.environment, "PYTHONUTF8") == "1" &&
            environmentValue(normalized.environment, "PYTHONIOENCODING") != nullptr &&
            *environmentValue(normalized.environment, "PYTHONIOENCODING") == "utf-8" &&
            forgeTest != nullptr && *forgeTest == "one" &&
            path != nullptr && containsAscii(*path, "system32") &&
            containsAscii(*path, "powershell\\7") &&
            containsAscii(*path, "git\\cmd") &&
            pathExt != nullptr && containsAscii(*pathExt, ".exe") &&
            comSpec != nullptr && containsAscii(*comSpec, "cmd.exe") &&
            normalized.inheritEnvironment,
        "Shell did not preserve its authorized envelope, toolchain PATH, and budgets");
    require(
        supervisor->authorityIds().front() == fixture.authority.authorityId() &&
            supervisor->projectIds().front() == fixture.projectId,
        "Shell did not keep its private execution authority bound to the caller identity");
    const auto& privateAuthority = supervisor->authorities().front();
    require(
        privateAuthority.callerId() == callerAuthority.callerId() &&
            privateAuthority.generation() == callerAuthority.generation() &&
            privateAuthority.grants() ==
                std::vector<Domain::FileAccess>{Domain::FileAccess::Execute} &&
            containsRoot(privateAuthority, fixture.workspaceRoot) &&
            containsRoot(privateAuthority, fixture.toolsRoot) &&
            !containsRoot(callerAuthority, fixture.toolsRoot),
        "Shell did not keep executable scope private while preserving caller identity");

    supervisor->enqueue(processResult());
    auto longTimeout = shellRequest(fixture, "Get-Location");
    longTimeout.timeout = 10min;
    require(
        shell.execute(longTimeout, callerAuthority, context(41U)).hasValue(),
        "Shell rejected a timeout that should be clamped");
    require(
        supervisor->requests().back().timeout == 120s,
        "Shell did not clamp timeout to 120 seconds");

    auto maximumCommand = shellRequest(fixture,
        std::string(WindowsShellService::MaximumCommandBytes, 'x'));
    supervisor->enqueue(processResult());
    static_cast<void>(take(shell.execute(maximumCommand, callerAuthority, context(43U))));
    const auto& maximumNormalized = supervisor->requests().back();
    require(maximumNormalized.arguments == normalized.arguments &&
                maximumNormalized.stdinUtf8.starts_with(maximumCommand.arguments.front()) &&
                maximumNormalized.stdinUtf8.size() > WindowsShellService::MaximumCommandBytes,
            "The maximum shell script was copied into argv or lost during stdin delivery");
    static_cast<void>(take(Domain::validateProcessRequest(maximumNormalized,
        Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB))));

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

class ShellEnvironmentVariableScope final {
public:
    ShellEnvironmentVariableScope(const wchar_t* const name, const wchar_t* const value)
        : name_{name}
    {
        const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
        if (required != 0U) {
            std::wstring saved(required, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(name, saved.data(), required);
            require(written < required, "Could not save shell environment fixture");
            saved.resize(written);
            original_ = std::move(saved);
        }
        require(::SetEnvironmentVariableW(name, value) != FALSE,
            "Could not set shell environment fixture");
    }
    ~ShellEnvironmentVariableScope()
    {
        static_cast<void>(::SetEnvironmentVariableW(name_.c_str(),
            original_ ? original_->c_str() : nullptr));
    }
    ShellEnvironmentVariableScope(const ShellEnvironmentVariableScope&) = delete;
    ShellEnvironmentVariableScope& operator=(const ShellEnvironmentVariableScope&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> original_;
};

void shellProfileEnvironmentIsBoundedAndExplicit()
{
    const ShellEnvironmentVariableScope username{L"USERNAME", L"forge-shell-user"};
    const ShellEnvironmentVariableScope userdomain{L"USERDOMAIN", L"forge-shell-domain"};
    const ShellEnvironmentVariableScope userprofile{L"USERPROFILE", L"C:\\forge-shell-profile"};
    const ShellEnvironmentVariableScope appdata{L"APPDATA", L"C:\\forge-shell-profile\\Roaming"};
    const ShellEnvironmentVariableScope localappdata{L"LOCALAPPDATA", L"C:\\forge-shell-profile\\Local"};
    const ShellEnvironmentVariableScope homedrive{L"HOMEDRIVE", L"C:"};
    const ShellEnvironmentVariableScope homepath{L"HOMEPATH", L"\\forge-shell-profile"};
    const ShellEnvironmentVariableScope secret{L"FORGE_SHELL_TEST_SECRET_TOKEN", L"canary-only"};
    const ShellEnvironmentVariableScope pythonUtf8{L"PYTHONUTF8", L"0"};
    const ShellEnvironmentVariableScope pythonEncoding{L"PYTHONIOENCODING", L"cp1252"};
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsShellService shell{fixture.powerShellExecutable, supervisor};
    const auto find = [](const Domain::ProcessRequest& request,
                         const std::string_view name) -> const std::string* {
        for (const auto& variable : request.environment) {
            if (variable.name == name) return &variable.value;
        }
        return nullptr;
    };
    static_cast<void>(take(shell.execute(shellRequest(fixture, "Get-Location"), fixture.authority, context(140U))));
    const auto& normalized = supervisor->requests().back();
    for (const auto& [name, expected] : std::vector<std::pair<std::string, std::string>>{
             {"USERNAME", "forge-shell-user"}, {"USERDOMAIN", "forge-shell-domain"},
             {"USERPROFILE", "C:\\forge-shell-profile"},
             {"APPDATA", "C:\\forge-shell-profile\\Roaming"},
             {"LOCALAPPDATA", "C:\\forge-shell-profile\\Local"},
             {"HOMEDRIVE", "C:"}, {"HOMEPATH", "\\forge-shell-profile"},
             {"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"}}) {
        const auto* value = find(normalized, name);
        require(value != nullptr && *value == expected,
            "Shell omitted or changed safe environment default: " + name);
    }
    require(find(normalized, "FORGE_SHELL_TEST_SECRET_TOKEN") == nullptr,
        "Shell copied a non-allowlisted host environment value");

    auto explicitRequest = shellRequest(fixture, "Get-Location");
    explicitRequest.environment = {{"username", "explicit-user"},
        {"pythonutf8", "0"}, {"pythonioencoding", "ascii"}};
    static_cast<void>(take(shell.execute(explicitRequest, fixture.authority, context(141U))));
    const auto& explicitNormalized = supervisor->requests().back();
    require(find(explicitNormalized, "username") != nullptr &&
            *find(explicitNormalized, "username") == "explicit-user" &&
            find(explicitNormalized, "USERNAME") == nullptr &&
            find(explicitNormalized, "pythonutf8") != nullptr &&
            *find(explicitNormalized, "pythonutf8") == "0" &&
            find(explicitNormalized, "PYTHONUTF8") == nullptr &&
            find(explicitNormalized, "pythonioencoding") != nullptr &&
            *find(explicitNormalized, "pythonioencoding") == "ascii" &&
            find(explicitNormalized, "PYTHONIOENCODING") == nullptr,
        "Shell changed explicit environment overrides or duplicated their names");

    const std::wstring oversized(Domain::MaximumProcessEnvironmentValueBytes + 1U, L'x');
    const ShellEnvironmentVariableScope oversizedDomain{L"USERDOMAIN", oversized.c_str()};
    const ShellEnvironmentVariableScope missingHome{L"HOMEPATH", nullptr};
    static_cast<void>(take(shell.execute(shellRequest(fixture, "Get-Location"), fixture.authority, context(142U))));
    require(find(supervisor->requests().back(), "USERDOMAIN") == nullptr &&
            find(supervisor->requests().back(), "HOMEPATH") == nullptr,
        "Shell imported an oversized value or fabricated a missing profile value");
}

void shellPolicyAuthorityCancellationAndShutdownFailClosed()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsShellService shell{
        fixture.powerShellExecutable, supervisor};

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
    auto malformed = shellRequest(fixture, std::string{"\xc3\x28", 2U});
    requireError(shell.execute(malformed, fixture.authority, context(54U)),
        Domain::ErrorCodes::InvalidRequest, "Shell accepted malformed script UTF-8");
    auto embeddedNul = shellRequest(fixture, std::string{"a\0b", 3U});
    requireError(shell.execute(embeddedNul, fixture.authority, context(55U)),
        Domain::ErrorCodes::InvalidRequest, "Shell accepted a NUL in script text");
    auto outside = shellRequest(fixture, "Get-Location");
    outside.workingDirectory = path("C:\\outside");
    requireError(
        shell.execute(outside, fixture.authority, context(53U)),
        Domain::ErrorCodes::PathOutsideAuthority,
        "Shell accepted an unauthorized working directory");

    Fakes::DeterministicWorkspaceAuthority disabledIssuer{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000003"),
        parse<Domain::ClientId>("p13-client"),
        {fixture.workspaceRoot},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {Domain::FileAccess::Delete},
        false,
        1U};
    const auto disabledAuthority = take(
        disabledIssuer.authorityFor(fixture.projectId, context(58U)));
    const auto disabledRepository = take(disabledIssuer.authorize(
        disabledAuthority,
        Domain::PathAuthorizationRequest{
            fixture.workspaceRoot,
            std::optional<Domain::PathText>{fixture.workspaceRoot},
            Domain::FileAccess::Read,
            false},
        context(59U)));
    WindowsGitService git{fixture.gitExecutable, supervisor};
    supervisor->enqueue(processResult());
    require(
        git.status(
            disabledRepository, disabledAuthority, 1'024U,
            context(60U)).hasValue(),
        "Git incorrectly inherited the user-shell execution policy");
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            disabledAuthority,
            context(61U)),
        Domain::ErrorCodes::ShellDisabled,
        "Shell ignored the disabled-by-default authority policy");

    const auto cancelledId = operationId(62U);
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
            context(63U)),
        Domain::ErrorCodes::Cancelled,
        "Shell did not preserve a supervisor cancellation error");

    const auto callsBeforeShutdown = supervisor->requests().size();
    shell.shutdown();
    requireError(
        shell.execute(
            shellRequest(fixture, "Get-Location"),
            fixture.authority,
            context(64U)),
        Domain::ErrorCodes::Cancelled,
        "Shell accepted work after shutdown");
    require(
        supervisor->requests().size() == callsBeforeShutdown &&
            supervisor->cancelAllCalls() == 0U &&
            supervisor->shutdownCalls() == 0U,
        "Shell shutdown affected the shared supervisor or dispatched new work");
}

void servicesRebindPrivateExecutionScopePerProject()
{
    AuthorityFixture projectA;
    const auto projectBRoot = path("C:\\workspace\\project-b");
    const auto projectBId = parse<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000002");
    Fakes::DeterministicWorkspaceAuthority projectBIssuer{
        parse<Domain::AuthorityId>(
            "30000000-0000-4000-8000-000000000022"),
        parse<Domain::ClientId>("p14-project-b-client"),
        {projectBRoot},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        7U};
    const auto projectBAuthority = take(
        projectBIssuer.authorityFor(projectBId, context(70U)));
    const auto projectBRepository = take(projectBIssuer.authorize(
        projectBAuthority,
        Domain::PathAuthorizationRequest{
            projectBRoot,
            std::optional<Domain::PathText>{projectBRoot},
            Domain::FileAccess::Read,
            false},
        context(71U)));

    auto supervisor = std::make_shared<ScriptedProcessSupervisor>();
    WindowsGitService git{projectA.gitExecutable, supervisor};
    require(
        git.status(
            projectA.readRepository, projectA.authority, 1'024U,
            context(72U)).hasValue() &&
            git.status(
                projectBRepository, projectBAuthority, 1'024U,
                context(73U)).hasValue(),
        "One Git service instance did not execute consecutive project scopes");
    requireError(
        git.status(
            projectBRepository, projectA.authority, 1'024U,
            context(74U)),
        Domain::ErrorCodes::Unauthorized,
        "Git accepted a cross-project authorized path");

    WindowsShellService shell{projectA.powerShellExecutable, supervisor};
    auto projectARequest = shellRequest(projectA, "Get-Location");
    Domain::ProcessRequest projectBRequest{projectA.powerShellExecutable};
    projectBRequest.arguments = {"Get-Location"};
    projectBRequest.workingDirectory = projectBRoot;
    require(
        shell.execute(
            projectARequest, projectA.authority, context(75U)).hasValue() &&
            shell.execute(
                projectBRequest, projectBAuthority, context(76U)).hasValue(),
        "One Shell service instance did not execute consecutive project scopes");

    const auto& requests = supervisor->requests();
    const auto& authorities = supervisor->authorities();
    require(
        requests.size() == 4U && authorities.size() == 4U &&
            requests[0].workingDirectory == projectA.workspaceRoot &&
            requests[1].workingDirectory == projectBRoot &&
            requests[2].workingDirectory == projectA.workspaceRoot &&
            requests[3].workingDirectory == projectBRoot,
        "Per-project native execution did not preserve each working directory");
    require(
        authorities[0].projectId() == projectA.projectId &&
            authorities[0].callerId() == projectA.authority.callerId() &&
            authorities[0].generation() == projectA.authority.generation() &&
            authorities[1].projectId() == projectBId &&
            authorities[1].authorityId() ==
                projectBAuthority.authorityId() &&
            authorities[1].callerId() == projectBAuthority.callerId() &&
            authorities[1].generation() == projectBAuthority.generation() &&
            authorities[2].projectId() == projectA.projectId &&
            authorities[3].projectId() == projectBId,
        "Per-project private authority identity or generation drifted");
    require(
        !containsRoot(projectA.authority, projectA.toolsRoot) &&
            !containsRoot(projectBAuthority, projectA.toolsRoot) &&
            containsRoot(authorities[0], projectA.toolsRoot) &&
            containsRoot(authorities[1], projectA.toolsRoot) &&
            containsRoot(authorities[2], projectA.toolsRoot) &&
            containsRoot(authorities[3], projectA.toolsRoot),
        "Executable authority leaked to callers or was omitted from private scopes");
}

void shellShutdownCancelsBeforeSupervisorAdmission()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<AdmissionBarrierProcessSupervisor>();
    auto shell = std::make_unique<WindowsShellService>(
        fixture.powerShellExecutable, supervisor);
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

[[nodiscard]] Domain::ShellJobSnapshot awaitJob(
    WindowsShellService& shell, const std::string_view id,
    const Contracts::WorkspaceAuthority& authority)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        auto snapshot = take(shell.getJob(id, authority, context(900U)));
        if (snapshot.state != Domain::ShellJobState::Running) return snapshot;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error{"Tracked job did not complete within the test deadline"};
}

void shellJobsOwnLifetimeAndBoundAdmission()
{
    AuthorityFixture fixture;
    auto supervisor = std::make_shared<JobProcessSupervisor>(JobProcessSupervisor::Mode::Block);
    WindowsShellService shell{fixture.powerShellExecutable, supervisor};
    require(shell.supportsJobs(), "Windows shell did not advertise tracked job support");
    auto request = shellRequest(fixture, "Write-Output job");
    request.timeout = 600s;
    std::stop_source callerCancellation;
    const Domain::OperationContext callContext{operationId(800U),
        std::chrono::steady_clock::now() + 100ms, callerCancellation.get_token(),
        parse<Domain::CorrelationId>("job-admission-call")};
    const auto first = take(shell.startJob(request, fixture.authority, callContext));
    require(supervisor->waitUntilStarted(1U), "Tracked job did not reach supervisor");
    callerCancellation.request_stop();
    require(first.jobId != callContext.operationId.value() &&
        first.state == Domain::ShellJobState::Running && !first.result,
        "Job did not return its independent identity and initial running state");
    require(supervisor->lastOperation().value() == first.jobId &&
        supervisor->lastDeadline() > callContext.deadline + 500s &&
        supervisor->lastRequest().timeout == 600s && supervisor->lastRequest().managedJob,
        "Tracked job reused the admission call deadline or synchronous timeout limit");
    std::this_thread::sleep_for(120ms);
    require(take(shell.getJob(first.jobId, fixture.authority, context(801U))).state ==
        Domain::ShellJobState::Running,
        "Admission cancellation/deadline cancelled the independently tracked job");

    const auto second = take(shell.startJob(request, fixture.authority, context(802U)));
    require(first.jobId != second.jobId && supervisor->waitUntilStarted(2U),
        "Tracked jobs did not receive distinct independent operation IDs");
    requireError(shell.startJob(request, fixture.authority, context(803U)),
        Domain::ErrorCodes::RateLimited, "Shell admitted more than two active jobs");
    const auto otherProject = parse<Domain::ProjectId>("20000000-0000-4000-8000-000000000099");
    const auto otherAuthority = take(fixture.issuer.authorityFor(otherProject, context(804U)));
    requireError(shell.getJob(first.jobId, otherAuthority, context(805U)),
        Domain::ErrorCodes::Unauthorized, "Another project read a shell job");
    requireError(shell.cancelJob(first.jobId, otherAuthority, context(806U)),
        Domain::ErrorCodes::Unauthorized, "Another project cancelled a shell job");
    require(take(shell.listJobs(otherAuthority, context(807U))).empty(),
        "Shell job list leaked another project's jobs");
    require(take(shell.listJobs(fixture.authority, context(808U))).size() == 2U,
        "Project could not rediscover its tracked jobs");
    static_cast<void>(take(shell.cancelJob(first.jobId, fixture.authority, context(809U))));
    const auto cancelled = awaitJob(shell, first.jobId, fixture.authority);
    require(cancelled.state == Domain::ShellJobState::Cancelled && cancelled.result &&
        cancelled.result->cancelled, "Explicit job cancellation was not retained");
    require(take(shell.cancelJob(first.jobId, fixture.authority, context(810U))).state ==
        Domain::ShellJobState::Cancelled, "Completed cancellation was not idempotent");
    shell.shutdown();
    require(take(shell.getJob(second.jobId, fixture.authority, context(811U))).state ==
        Domain::ShellJobState::Cancelled, "Shutdown did not cancel and join tracked jobs");
    requireError(shell.startJob(request, fixture.authority, context(812U)),
        Domain::ErrorCodes::Cancelled, "Shell accepted a job after shutdown");
}

void shellJobOutcomesAndRetentionRemainBounded()
{
    AuthorityFixture fixture;
    auto request = shellRequest(fixture, "Write-Output job");
    request.timeout = 600s;
    for (const auto mode : {JobProcessSupervisor::Mode::Complete,
        JobProcessSupervisor::Mode::Nonzero, JobProcessSupervisor::Mode::Timeout,
        JobProcessSupervisor::Mode::Error}) {
        auto supervisor = std::make_shared<JobProcessSupervisor>(mode);
        WindowsShellService shell{fixture.powerShellExecutable, supervisor};
        const auto started = take(shell.startJob(request, fixture.authority, context(820U)));
        const auto completed = awaitJob(shell, started.jobId, fixture.authority);
        const auto repeated = take(shell.getJob(started.jobId, fixture.authority, context(821U)));
        require(repeated.state == completed.state && repeated.elapsed == completed.elapsed,
            "Shell job status consumed or changed completed evidence");
        if (mode == JobProcessSupervisor::Mode::Error) {
            require(completed.state == Domain::ShellJobState::Failed && completed.error &&
                completed.error->code == Domain::ErrorCodes::ProcessLaunchFailed && !completed.result,
                "Typed supervisor failure was discarded");
        } else {
            const auto expected = mode == JobProcessSupervisor::Mode::Complete
                ? Domain::ShellJobState::Completed : mode == JobProcessSupervisor::Mode::Timeout
                    ? Domain::ShellJobState::TimedOut : Domain::ShellJobState::Failed;
            require(completed.state == expected && completed.result &&
                completed.result->stdoutUtf8.size() == WindowsShellService::MaximumOutputBytes &&
                completed.result->stdoutTruncated && repeated.result &&
                repeated.result->stdoutUtf8 == completed.result->stdoutUtf8,
                "Tracked job outcome or bounded final output was not retained");
        }
    }

    auto supervisor = std::make_shared<JobProcessSupervisor>(JobProcessSupervisor::Mode::Complete);
    WindowsShellService shell{fixture.powerShellExecutable, supervisor};
    std::string oldest;
    for (std::uint32_t index = 0U; index < 18U; ++index) {
        const auto started = take(shell.startJob(request, fixture.authority, context(830U + index)));
        if (index == 0U) oldest = started.jobId;
        static_cast<void>(awaitJob(shell, started.jobId, fixture.authority));
    }
    require(take(shell.listJobs(fixture.authority, context(850U))).size() ==
        WindowsShellService::MaximumRetainedJobs, "Completed shell results exceeded retention limit");
    requireError(shell.getJob(oldest, fixture.authority, context(851U)),
        Domain::ErrorCodes::RecordNotFound, "Oldest completed job was not evicted");
    request.timeout = 3'601s;
    requireError(shell.startJob(request, fixture.authority, context(852U)),
        Domain::ErrorCodes::InvalidRequest, "Shell accepted a job beyond one hour");
    request.timeout = 0ms;
    requireError(shell.startJob(request, fixture.authority, context(853U)),
        Domain::ErrorCodes::InvalidRequest, "Shell accepted a nonpositive job timeout");
    request.timeout = 600s;
    request.managedJob = true;
    requireError(shell.execute(request, fixture.authority, context(854U)),
        Domain::ErrorCodes::InvalidRequest, "Ordinary shell execution bypassed tracking with managed flag");
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
        shellProfileEnvironmentIsBoundedAndExplicit();
        std::cout << "PASS native_tools.shell_profile_environment\n";
        shellPolicyAuthorityCancellationAndShutdownFailClosed();
        std::cout << "PASS native_tools.shell_policy_shutdown\n";
        servicesRebindPrivateExecutionScopePerProject();
        std::cout << "PASS native_tools.per_project_execution_scope\n";
        shellShutdownCancelsBeforeSupervisorAdmission();
        std::cout << "PASS native_tools.shell_admission_shutdown_race\n";
        shellJobsOwnLifetimeAndBoundAdmission();
        std::cout << "PASS native_tools.shell_job_lifetime_admission\n";
        shellJobOutcomesAndRetentionRemainBounded();
        std::cout << "PASS native_tools.shell_job_outcomes_retention\n";
        std::cout << "SUMMARY passed=10 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
