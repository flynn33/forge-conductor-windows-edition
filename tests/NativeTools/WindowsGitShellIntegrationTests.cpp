#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "NativeTools/Windows/ShellJobStorage.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;
namespace NativeTools = ForgeConductor::NativeTools::Windows;

void require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::PathText pathText(
    const std::filesystem::path& value)
{
    return take(InfrastructureDetail::WindowsPathResolver::toPathText(
        std::filesystem::absolute(value).wstring()));
}

[[nodiscard]] Domain::OperationContext context(const std::uint32_t index)
{
    char operation[37]{};
    const auto written = std::snprintf(
        operation, sizeof(operation),
        "%08x-0000-4000-8000-%012x", index, index);
    require(written == 36, "operation identifier formatting failed");
    return Domain::OperationContext{
        parse<Domain::OperationId>(operation),
        std::chrono::steady_clock::now() + 30s,
        {},
        parse<Domain::CorrelationId>("p13-native-tool-integration")};
}

class TemporaryWorkspace final {
public:
    TemporaryWorkspace()
    {
        LARGE_INTEGER counter{};
        require(::QueryPerformanceCounter(&counter) != FALSE,
                "QueryPerformanceCounter failed");
        path_ = std::filesystem::temp_directory_path() /
            (L"ForgeConductor.P13.NativeTools." +
             std::to_wstring(::GetCurrentProcessId()) + L"." +
             std::to_wstring(counter.QuadPart));
        require(std::filesystem::create_directory(path_),
                "temporary workspace creation failed");
    }

    ~TemporaryWorkspace() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    TemporaryWorkspace(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace& operator=(const TemporaryWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] Contracts::WorkspaceAuthority makeAuthority(
    const Domain::AuthorityId& authorityId,
    const Domain::ProjectId& projectId,
    const Domain::ClientId& clientId,
    const std::vector<Domain::PathText>& roots,
    const Domain::OperationContext& operation)
{
    Fakes::DeterministicWorkspaceAuthority issuer{
        authorityId,
        clientId,
        roots,
        Domain::FileAccess::Execute,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        {},
        true,
        1U};
    issuer.setNow(std::chrono::steady_clock::now());
    return take(issuer.authorityFor(projectId, operation));
}

[[nodiscard]] Contracts::AuthorizedPath authorize(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& target,
    const Domain::PathText& root,
    const Domain::FileAccess access,
    const std::uint32_t operationIndex)
{
    Fakes::DeterministicWorkspaceAuthority issuer{
        authority.authorityId(),
        authority.callerId(),
        authority.trustedRoots(),
        authority.intent(),
        authority.grants(),
        authority.denials(),
        authority.shellEnabled(),
        authority.generation()};
    issuer.setNow(std::chrono::steady_clock::now());
    return take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{target, root, access, false},
        context(operationIndex)));
}

[[nodiscard]] Domain::ProcessResult runSetupGit(
    Contracts::IProcessSupervisor& supervisor,
    const Domain::PathText& gitExecutable,
    const Domain::PathText& workspace,
    const Contracts::WorkspaceAuthority& authority,
    std::vector<std::string> arguments,
    const std::uint32_t operationIndex)
{
    Domain::ProcessRequest request{gitExecutable};
    request.arguments = std::move(arguments);
    request.workingDirectory = workspace;
    request.environment = {{"GIT_TERMINAL_PROMPT", "0"}};
    request.timeout = 10s;
    request.maximumStdoutBytes = 80'000U;
    request.maximumStderrBytes = 20'000U;
    auto result = take(supervisor.run(
        request, authority, context(operationIndex)));
    require(result.exitCode == 0 && !result.timedOut && !result.cancelled,
            "Git repository setup process failed");
    return result;
}

[[nodiscard]] Domain::ShellJobSnapshot admissionSnapshot(const std::uint32_t index,
    const Domain::PathText& root)
{
    return Domain::ShellJobSnapshot{context(index).operationId.value(), Domain::ShellJobState::Running,
        "persisted admission fixture", root.value(), 30U, std::nullopt, std::nullopt, 0ms};
}

int admissionChild(const char* const argv[])
{
    using Storage = NativeTools::Detail::ShellJobStorage;
    const auto root = take(Domain::PathText::create(argv[2]));
    const auto project = parse<Domain::ProjectId>(argv[3]);
    const auto widen = [](const std::string_view text) { return std::wstring{text.begin(), text.end()}; };
    InfrastructureDetail::UniqueHandle start{::OpenEventW(SYNCHRONIZE, FALSE, widen(argv[5]).c_str())};
    InfrastructureDetail::UniqueHandle release{::OpenEventW(SYNCHRONIZE, FALSE, widen(argv[6]).c_str())};
    require(start && release && ::WaitForSingleObject(start.get(), 10'000U) == WAIT_OBJECT_0,
        "admission child did not receive its start gate");
    auto snapshot = admissionSnapshot(100U, root);
    snapshot.jobId = argv[4];
    auto created = Storage::create(root, project, snapshot, context(101U));
    {
        std::ofstream output{std::filesystem::path{argv[7]}, std::ios::binary};
        output << (created ? "admitted" : created.error().code);
        output.close();
        require(output.good(), "admission child outcome could not be written");
    }
    require(::WaitForSingleObject(release.get(), 10'000U) == WAIT_OBJECT_0,
        "admission child did not receive its release gate");
    return EXIT_SUCCESS;
}

class AdmissionChildOwner final {
public:
    AdmissionChildOwner(const std::wstring& arguments, const HANDLE release) : release_{release}
    {
        std::vector<wchar_t> executable(32U * 1024U);
        const auto length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length > 0U && length < executable.size(), "admission fixture executable cannot be located");
        std::wstring command = L"\"" + std::wstring{executable.data(), length} + L"\" " + arguments;
        STARTUPINFOW startup{}; startup.cb = static_cast<DWORD>(sizeof(startup));
        PROCESS_INFORMATION process{};
        require(::CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "admission fixture child cannot be started");
        process_.reset(process.hProcess);
        InfrastructureDetail::UniqueHandle thread{process.hThread};
        static_cast<void>(thread);
    }
    ~AdmissionChildOwner() noexcept
    {
        static_cast<void>(::SetEvent(release_));
        static_cast<void>(::WaitForSingleObject(process_.get(), 5'000U));
    }
    void join()
    {
        require(::WaitForSingleObject(process_.get(), 5'000U) == WAIT_OBJECT_0,
            "admission fixture child did not finish");
        DWORD exitCode{};
        require(::GetExitCodeProcess(process_.get(), &exitCode) != FALSE && exitCode == EXIT_SUCCESS,
            "admission fixture child reported failure");
    }
private:
    InfrastructureDetail::UniqueHandle process_;
    HANDLE release_{};
};

void exercisePersistedJobAdmission(const Domain::PathText& powerShellExecutable,
    const std::shared_ptr<Contracts::IProcessSupervisor>& supervisor)
{
    using Storage = NativeTools::Detail::ShellJobStorage;
    TemporaryWorkspace workspace;
    const auto root = pathText(workspace.path());
    const auto project = parse<Domain::ProjectId>("51515151-5151-4151-8151-515151515151");
    const auto folder = workspace.path() / project.value();
    std::vector<std::shared_ptr<Storage>> seeded;
    for (std::uint32_t index{1U}; index < Storage::MaximumPersistedJobs; ++index)
        seeded.push_back(take(Storage::create(root, project, admissionSnapshot(index, root), context(90U))));
    const auto malformedPath = folder / (admissionSnapshot(31U, root).jobId + ".json");
    { std::ofstream output{malformedPath, std::ios::binary | std::ios::app}; output << "tampered"; }
    const auto malformedBytes = std::filesystem::file_size(malformedPath);

    const auto prefix = L"Local\\ForgeConductor.JobAdmission.Test." + workspace.path().filename().wstring();
    const auto startName = prefix + L".start";
    const auto releaseName = prefix + L".release";
    InfrastructureDetail::UniqueHandle start{::CreateEventW(nullptr, TRUE, FALSE, startName.c_str())};
    InfrastructureDetail::UniqueHandle release{::CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str())};
    require(start && release, "admission fixture gates could not be created");
    const auto firstPath = workspace.path() / L"first.outcome";
    const auto secondPath = workspace.path() / L"second.outcome";
    const auto firstId = context(70U).operationId.value();
    const auto secondId = context(71U).operationId.value();
    const auto arguments = [&](const std::string& id, const std::filesystem::path& output) {
        return L"--job-admission-child \"" + workspace.path().wstring() + L"\" " +
            std::wstring{project.value().begin(), project.value().end()} + L" " +
            std::wstring{id.begin(), id.end()} + L" " + startName + L" " + releaseName +
            L" \"" + output.wstring() + L"\"";
    };
    AdmissionChildOwner first{arguments(firstId, firstPath), release.get()};
    AdmissionChildOwner second{arguments(secondId, secondPath), release.get()};
    require(::SetEvent(start.get()) != FALSE, "admission fixture start gate failed");
    const auto readOutcome = [](const std::filesystem::path& path) {
        std::ifstream input{path, std::ios::binary}; std::string text; input >> text; return text;
    };
    std::string firstOutcome, secondOutcome;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        firstOutcome = readOutcome(firstPath); secondOutcome = readOutcome(secondPath);
        if (!firstOutcome.empty() && !secondOutcome.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    require((firstOutcome == "admitted" && secondOutcome == Domain::ErrorCodes::RateLimited) ||
            (secondOutcome == "admitted" && firstOutcome == Domain::ErrorCodes::RateLimited),
        "concurrent job owners did not admit exactly one final slot and reject the other with rate_limited");
    const auto receiptCount = [&] {
        std::size_t count{};
        for (const auto& entry : std::filesystem::directory_iterator{folder})
            if (entry.path().extension() == L".json") ++count;
        return count;
    };
    require(receiptCount() == Storage::MaximumPersistedJobs,
        "cross-process admission exceeded the 32 receipt limit");
    require(std::filesystem::file_size(malformedPath) == malformedBytes,
        "admission removed or rewrote an unverified receipt instead of preserving its occupied slot");
    const auto rejectedId = firstOutcome == "admitted" ? secondId : firstId;
    require(!std::filesystem::exists(folder / (rejectedId + ".json")) &&
            !std::filesystem::exists(folder / (rejectedId + ".stdout.log")),
        "rejected persisted admission created process evidence artifacts");

    const auto authority = makeAuthority(parse<Domain::AuthorityId>("52525252-5252-4252-8252-525252525252"),
        project, parse<Domain::ClientId>("job-admission-client"), {root}, context(92U));
    NativeTools::WindowsShellService service{powerShellExecutable, supervisor, root};
    Domain::ProcessRequest request{powerShellExecutable};
    request.arguments = {"Write-Output 'must-not-run'"}; request.workingDirectory = root; request.timeout = 5s;
    const auto full = service.startJob(request, authority, context(93U));
    require(!full && full.error().code == Domain::ErrorCodes::RateLimited && full.error().retryable,
        "shell service did not preserve the cross-owner admission limit error");
    require(receiptCount() == Storage::MaximumPersistedJobs,
        "failed shell-service admission mutated durable receipt count");
    service.shutdown();

    require(::SetEvent(release.get()) != FALSE, "admission fixture release gate failed");
    first.join(); second.join();
    auto recovered = take(Storage::create(root, project, admissionSnapshot(72U, root), context(94U)));
    require(receiptCount() == Storage::MaximumPersistedJobs,
        "an ended owner did not free a slot through verified retention");
    auto completed = admissionSnapshot(1U, root); completed.state = Domain::ShellJobState::Completed;
    seeded.front()->persist(completed);
    auto replacement = take(Storage::create(root, project, admissionSnapshot(73U, root), context(95U)));
    require(!std::filesystem::exists(folder / (completed.jobId + ".json")),
        "completed receipt was not evicted to admit its replacement");
    bool resurrectionRejected{};
    try { seeded.front()->persist(completed); }
    catch (const std::exception&) { resurrectionRejected = true; }
    require(resurrectionRejected && receiptCount() == Storage::MaximumPersistedJobs &&
            !std::filesystem::exists(folder / (completed.jobId + ".json")),
        "a late terminal publication resurrected an evicted receipt beyond the project limit");
    auto expired = context(96U); expired.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto timedOut = Storage::create(root, project, admissionSnapshot(74U, root), expired);
    require(!timedOut && timedOut.error().code == Domain::ErrorCodes::DeadlineExceeded &&
            receiptCount() == Storage::MaximumPersistedJobs,
        "expired persisted admission did not preserve deadline failure without writes");
    static_cast<void>(recovered); static_cast<void>(replacement);
}

void exerciseGitAndShell(
    const std::filesystem::path& gitPath,
    const std::filesystem::path& powerShellPath)
{
    require(std::filesystem::is_regular_file(gitPath),
            "configured Git executable is missing");
    require(std::filesystem::is_regular_file(powerShellPath),
            "configured PowerShell executable is missing");

    TemporaryWorkspace workspace;
    const auto workspacePath = pathText(workspace.path());
    const auto gitExecutable = pathText(gitPath);
    const auto gitRoot = pathText(gitPath.parent_path());
    const auto powerShellExecutable = pathText(powerShellPath);

    Infrastructure::SystemClock clock;
    const auto budgets = Domain::budgetsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    auto diagnostics = std::make_shared<Infrastructure::WindowsRuntimeDiagnostics>(
        clock, budgets);
    auto supervisor = std::make_shared<Infrastructure::WindowsProcessSupervisor>(
        budgets, diagnostics);
    exercisePersistedJobAdmission(powerShellExecutable, supervisor);

    const auto clientId =
        parse<Domain::ClientId>("p13-native-tool-integration-client");
    const auto gitProject = parse<Domain::ProjectId>(
        "61616161-6161-4161-8161-616161616161");
    const auto gitSetupAuthority = makeAuthority(
        parse<Domain::AuthorityId>(
            "70707070-7070-4070-8070-707070707070"),
        gitProject,
        clientId,
        {workspacePath, gitRoot},
        context(19U));
    const auto gitAuthority = makeAuthority(
        parse<Domain::AuthorityId>(
            "71717171-7171-4171-8171-717171717171"),
        gitProject,
        clientId,
        {workspacePath},
        context(1U));

    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitSetupAuthority,
        {"init", "--quiet"}, 2U));
    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitSetupAuthority,
        {"config", "user.name", "Forge Conductor Test"}, 3U));
    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitSetupAuthority,
        {"config", "user.email", "forge-conductor-test@invalid.example"}, 4U));

    const auto trackedFile = workspace.path() / L"tracked.txt";
    {
        std::ofstream output{trackedFile, std::ios::binary};
        output << "native Git adapter integration\n";
        require(output.good(), "Git integration fixture write failed");
    }

    const auto repositoryRead = authorize(
        gitAuthority, workspacePath, workspacePath,
        Domain::FileAccess::Read, 5U);
    const auto repositoryWrite = authorize(
        gitAuthority, workspacePath, workspacePath,
        Domain::FileAccess::Write, 6U);
    const auto fileRead = authorize(
        gitAuthority, pathText(trackedFile), workspacePath,
        Domain::FileAccess::Read, 7U);
    NativeTools::WindowsGitService git{
        gitExecutable, supervisor};

    const auto status = take(git.status(
        repositoryRead, gitAuthority, 8'192U, context(8U)));
    require(status.exitCode == 0 &&
                status.stdoutUtf8.find("tracked.txt") != std::string::npos,
            "Git status omitted the untracked fixture");
    const auto added = take(git.add(
        repositoryWrite,
        gitAuthority,
        std::span<const Contracts::AuthorizedPath>{&fileRead, 1U},
        context(9U)));
    require(added.exitCode == 0,
            "Git add failed through the production adapter");
    const std::vector<std::string> cached{"--cached"};
    const auto diff = take(git.diff(
        repositoryRead, gitAuthority, cached, 16'384U, context(10U)));
    require(diff.exitCode == 0 &&
                diff.stdoutUtf8.find("native Git adapter integration") !=
                    std::string::npos,
            "Git cached diff omitted the fixture content");
    const auto committed = take(git.commit(
        repositoryWrite, gitAuthority, "P13 native Git integration",
        context(11U)));
    require(committed.exitCode == 0,
            "Git commit returned a nonzero process outcome");
    const auto log = take(git.log(
        repositoryRead, gitAuthority, 1U, 8'192U, context(12U)));
    require(log.exitCode == 0 &&
                log.stdoutUtf8.find("P13 native Git integration") !=
                    std::string::npos,
            "Git log omitted the committed message");
    const auto nonzero = take(git.commit(
        repositoryWrite, gitAuthority, "P13 no-op commit", context(13U)));
    require(nonzero.exitCode != 0 &&
                (!nonzero.stdoutUtf8.empty() || !nonzero.stderrUtf8.empty()),
            "Git integration discarded a real nonzero process payload");

    const auto shellProject = parse<Domain::ProjectId>(
        "81818181-8181-4181-8181-818181818181");
    const auto shellAuthority = makeAuthority(
        parse<Domain::AuthorityId>(
            "91919191-9191-4191-8191-919191919191"),
        shellProject,
        clientId,
        {workspacePath},
        context(14U));
    NativeTools::WindowsShellService shell{
        powerShellExecutable, supervisor};

    Domain::ProcessRequest shellRequest{powerShellExecutable};
    shellRequest.arguments = {"Write-Output 'p13-shell-ok'"};
    shellRequest.workingDirectory = workspacePath;
    shellRequest.timeout = 10s;
    shellRequest.maximumStdoutBytes = 1'024U;
    shellRequest.maximumStderrBytes = 1'024U;
    const auto shellResult = take(shell.execute(
        shellRequest, shellAuthority, context(15U)));
    require(shellResult.exitCode == 0 &&
                shellResult.stdoutUtf8.find("p13-shell-ok") != std::string::npos,
            "PowerShell adapter did not return expected output");
    require(shellResult.stdoutUtf8.find('\0') == std::string::npos &&
                shellResult.stderrUtf8.find('\0') == std::string::npos,
            "PowerShell adapter returned embedded NUL bytes");

    // A real report-sized here-string crosses both the per-argument bound and
    // CreateProcess's command-line bound. CRLF, Unicode and quotes stay literal.
    const std::string unicode{"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xe2\x82\xac"};
    const std::string reportSuffix = "\r\n" + unicode + " 'quotes' `$literal";
    const std::string scriptPrefix = "$report=@'\r\n";
    const std::string scriptSuffix = reportSuffix + "\r\n'@\r\n"
        "if (!$report.EndsWith('" + unicode + " ''quotes'' `$literal')) {throw 'report corrupted'}\r\n"
        "Write-Output ([System.Text.Encoding]::UTF8.GetByteCount($report))\r\n# trailing comment";
    const auto paddingBytes = NativeTools::WindowsShellService::MaximumCommandBytes -
        scriptPrefix.size() - scriptSuffix.size();
    const auto largeScript = scriptPrefix + std::string(paddingBytes, 'x') + scriptSuffix;
    require(largeScript.size() == NativeTools::WindowsShellService::MaximumCommandBytes,
            "The real shell boundary fixture must contain exactly 65536 UTF-8 bytes");
    shellRequest.arguments = {largeScript};
    const auto largeResult = take(shell.execute(shellRequest, shellAuthority, context(62U)));
    const auto expectedReportBytes = std::to_string(paddingBytes + reportSuffix.size());
    require(largeResult.exitCode == 0 &&
                largeResult.stdoutUtf8.find(expectedReportBytes) != std::string::npos,
            "A maximum-size multiline Unicode report did not execute intact via stdin");
    shellRequest.arguments = {largeScript + "x"};
    const auto oversizedScript = shell.execute(shellRequest, shellAuthority, context(63U));
    require(!oversizedScript && oversizedScript.error().code == Domain::ErrorCodes::PayloadTooLarge,
            "A real shell accepted a script over 64 KiB");
    for (const auto& [command, exitCode] : std::vector<std::pair<std::string, std::int32_t>>{
             {"exit 0 # explicit success", 0},
             {"exit 23 # explicit exit", 23}, {"throw 'script failure'", 1},
             {"Write-Error 'script failure'\r\n\r\n# comment", 1},
             {"cmd /c exit 7 # native error", 1},
             {"cmd /c exit 7; Write-Error 'script failure'", 1},
             {"cmd /c exit 7; Get-Item -LiteralPath 'missing-shell-item'", 1},
             {"cmd /c exit 7\r\nWrite-Error 'script failure'\r\n# comment", 1},
             {"cmd /c exit 7; Write-Output 'recovered'", 0},
             {"cmd /c exit 7; $value = 123", 0},
             {"& {Write-Error 'nested failure'}", 0}}) {
      shellRequest.arguments = {command};
      const auto failureResult = take(shell.execute(shellRequest, shellAuthority, context(64U)));
      require(failureResult.exitCode == exitCode,
              "Script stdin delivery changed -Command exit status: " + command);
    }

    shellRequest.arguments = {"Start-Sleep -Seconds 2"};
    shellRequest.timeout = 100ms;
    const auto timedOut = take(shell.execute(
        shellRequest, shellAuthority, context(16U)));
    require(timedOut.timedOut,
            "PowerShell adapter did not enforce its requested timeout");

    shellRequest.arguments = {"Write-Output ('x' * 4096)"};
    shellRequest.timeout = 10s;
    shellRequest.maximumStdoutBytes = 32U;
    const auto bounded = take(shell.execute(
        shellRequest, shellAuthority, context(17U)));
    require(bounded.stdoutTruncated && bounded.stdoutUtf8.size() == 32U,
            "PowerShell adapter did not enforce its stdout cap");

    const auto waitForJob = [&](const std::string_view jobId) {
        const auto jobDeadline = std::chrono::steady_clock::now() + 10s;
        do {
            auto snapshot = take(shell.getJob(jobId, shellAuthority, context(30U)));
            if (snapshot.state != Domain::ShellJobState::Running) return snapshot;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < jobDeadline);
        throw std::runtime_error{"Real tracked PowerShell job did not complete"};
    };
    shellRequest.arguments = {largeScript};
    shellRequest.maximumStdoutBytes = 1'024U;
    const auto largeJob = take(shell.startJob(shellRequest, shellAuthority, context(65U)));
    const auto largeJobResult = waitForJob(largeJob.jobId);
    require(largeJobResult.state == Domain::ShellJobState::Completed && largeJobResult.result &&
                largeJobResult.result->exitCode == 0 &&
                largeJobResult.result->stdoutUtf8.find(expectedReportBytes) != std::string::npos,
            "A tracked maximum-size script did not preserve report content and completion");
    shellRequest.arguments = {"Start-Sleep -Milliseconds 200; Write-Output 'tracked-job-ok'"};
    shellRequest.timeout = 10s;
    shellRequest.maximumStdoutBytes = 1'024U;
    const Domain::OperationContext admissionContext{context(31U).operationId,
        std::chrono::steady_clock::now() + 100ms, {}, context(31U).correlationId};
    const auto asynchronous = take(shell.startJob(shellRequest, shellAuthority, admissionContext));
    const auto asynchronousResult = waitForJob(asynchronous.jobId);
    require(asynchronousResult.state == Domain::ShellJobState::Completed &&
        asynchronousResult.result && asynchronousResult.result->exitCode == 0 &&
        asynchronousResult.result->stdoutUtf8.find("tracked-job-ok") != std::string::npos,
        "Real tracked job did not survive its admission-call deadline and return final output");
    shellRequest.arguments = {"Start-Sleep -Seconds 30"};
    shellRequest.timeout = 100ms;
    const auto timedJob = take(shell.startJob(shellRequest, shellAuthority, context(32U)));
    const auto timedJobResult = waitForJob(timedJob.jobId);
    require(timedJobResult.state == Domain::ShellJobState::TimedOut && timedJobResult.result &&
        timedJobResult.result->timedOut && timedJobResult.result->terminationConfirmed,
        "Real tracked job timeout did not retain confirmed process-tree termination");
    shellRequest.timeout = 60s;
    const auto cancelledJob = take(shell.startJob(shellRequest, shellAuthority, context(33U)));
    static_cast<void>(take(shell.cancelJob(cancelledJob.jobId, shellAuthority, context(34U))));
    const auto cancelledJobResult = waitForJob(cancelledJob.jobId);
    require(cancelledJobResult.state == Domain::ShellJobState::Cancelled,
        "Real tracked PowerShell job did not terminate after explicit cancellation");

    const auto jobRoot = pathText(workspace.path() / L"jobs");
    NativeTools::WindowsShellService durable{powerShellExecutable, supervisor, jobRoot};
    std::atomic<unsigned> completionCalls{};
    durable.setJobCompletionSink([&](const Domain::ProjectId& project, const Domain::ShellJobSnapshot& snapshot) {
        require(project == shellProject && snapshot.result && !snapshot.logHash.empty(),
            "completion callback did not receive final project-scoped process evidence");
        completionCalls.fetch_add(1U);
    });
    const auto waitForDurable = [&](NativeTools::WindowsShellService& service, const std::string_view id) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        do {
            auto snapshot = take(service.getJob(id, shellAuthority, context(40U)));
            if (snapshot.state != Domain::ShellJobState::Running) return snapshot;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error{"Durable process job did not complete"};
    };
    Domain::ProcessRequest direct{powerShellExecutable};
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command",
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
        "[Console]::InputEncoding=[Text.UTF8Encoding]::new($false); "
        "[Console]::Write([Console]::In.ReadToEnd()); Write-Output 'live-log'; "
        "Start-Sleep -Milliseconds 800; Write-Output ('z' * 100000); Write-Output $env:FORGE_JOB_VALUE; "
        "[Console]::Error.WriteLine('stderr-proof')"};
    direct.workingDirectory = workspacePath;
    direct.environment = {{"FORGE_JOB_VALUE", "explicit-env-proof"}};
    direct.stdinUtf8 = "direct-stdin \xe2\x82\xac\n";
    direct.timeout = 10s;
    direct.maximumStdoutBytes = 1'024U; direct.maximumStderrBytes = 1'024U;
    const auto directJob = take(durable.startProcess(direct, shellAuthority, context(41U)));
    require(directJob.processId != 0U && directJob.processId != ::GetCurrentProcessId() &&
                directJob.processCreationTime != 0U && !directJob.stdoutPath.empty() && !directJob.stderrPath.empty(),
            "durable launch did not return the actual child identity and evidence paths");
    bool readLive{};
    const auto liveDeadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < liveDeadline) {
        const auto durableStatus = take(durable.getJob(directJob.jobId, shellAuthority, context(42U)));
        const auto page = take(durable.readJobLog(directJob.jobId, false, 0U, 0U, shellAuthority, context(43U)));
        if (durableStatus.state == Domain::ShellJobState::Running && durableStatus.processAlive == true &&
            page.text.find("live-log") != std::string::npos) {
            readLive = true; break;
        }
        if (durableStatus.state != Domain::ShellJobState::Running) break;
        std::this_thread::sleep_for(10ms);
    }
    require(readLive, "durable stdout was not readable while the process was running");
    const auto durableResult = waitForDurable(durable, directJob.jobId);
    require(durableResult.state == Domain::ShellJobState::Completed && durableResult.result &&
                durableResult.result->stdoutTruncated && durableResult.result->stdoutUtf8.size() == 1'024U &&
                durableResult.memoryAttached && !durableResult.memoryAttachError &&
                !durableResult.logHash.empty() && durableResult.arguments == direct.arguments,
            "durable job lost bounded result output, argv, receipt hash, or memory attachment");
    const auto firstPage = take(durable.readJobLog(directJob.jobId, false, 0U, 0U, shellAuthority, context(44U)));
    require(firstPage.text.size() == 32U * 1024U && firstPage.hasMore && firstPage.totalBytes > 100'000U &&
                firstPage.nextOffset == firstPage.text.size() && !firstPage.textLossy,
            "durable logs did not retain output beyond the bounded result capture or page it correctly");
    require(firstPage.text.starts_with(direct.stdinUtf8),
            "Direct process normalization dropped or changed the UTF-8 stdin payload");
    direct.stdinUtf8.clear();
    const auto lastLines = take(durable.readJobLog(directJob.jobId, false, std::nullopt, 1U, shellAuthority, context(45U)));
    require(lastLines.text.find("explicit-env-proof") != std::string::npos,
            "direct process launch did not preserve explicit environment variables");
    const auto stderrPage = take(durable.readJobLog(directJob.jobId, true, std::nullopt, 100U, shellAuthority, context(46U)));
    require(stderrPage.text.find("stderr-proof") != std::string::npos,
            "durable stderr log omitted process output");
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool completionEntered{}, completionReleased{};
    std::atomic<unsigned> blockedCompletionCalls{};
    durable.setJobCompletionSink([&](const Domain::ProjectId&, const Domain::ShellJobSnapshot&) {
        std::unique_lock lock{completionMutex};
        completionEntered = true;
        blockedCompletionCalls.fetch_add(1U);
        completionCondition.notify_all();
        completionCondition.wait(lock, [&] { return completionReleased; });
    });
    const auto releaseCompletion = [&] {
        { std::scoped_lock lock{completionMutex}; completionReleased = true; }
        completionCondition.notify_all();
    };
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command", "Write-Output 'completion-blocked'"};
    Domain::ShellJobSnapshot blockedCompletion;
    std::string blockedJobId;
    try {
        blockedJobId = take(durable.startProcess(direct, shellAuthority, context(62U))).jobId;
        {
            std::unique_lock lock{completionMutex};
            require(completionCondition.wait_for(lock, 10s, [&] { return completionEntered; }),
                "Process completion sink did not enter its bounded fixture wait");
        }
        blockedCompletion = take(durable.getJob(blockedJobId, shellAuthority, context(63U)));
    } catch (...) {
        releaseCompletion();
        throw;
    }
    releaseCompletion();
    const auto releasedCompletion = waitForDurable(durable, blockedJobId);
    require(blockedCompletion.state == Domain::ShellJobState::Running && blockedCompletion.processAlive == false &&
                releasedCompletion.state == Domain::ShellJobState::Completed && releasedCompletion.processAlive == false &&
                blockedCompletionCalls.load() == 1U,
            "Job finalization state incorrectly claimed an exited child PID was still alive");
    durable.setJobCompletionSink([&](const Domain::ProjectId&, const Domain::ShellJobSnapshot&) {
        completionCalls.fetch_add(1U); throw std::runtime_error{"memory fixture unavailable"};
    });
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command", "Write-Output 'attachment-failure'"};
    const auto attachmentJob = take(durable.startProcess(direct, shellAuthority, context(47U)));
    const auto attachmentResult = waitForDurable(durable, attachmentJob.jobId);
    require(attachmentResult.state == Domain::ShellJobState::Completed && attachmentResult.result &&
                attachmentResult.result->exitCode == 0 && !attachmentResult.memoryAttached &&
                attachmentResult.memoryAttachError && completionCalls.load() == 2U,
            "memory attachment failure discarded the process outcome or was not recorded exactly once");
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command",
        "[Console]::Out.Write(([string][char]1) * 80000); [Console]::Error.Write(([string][char]1) * 20000)"};
    direct.maximumStdoutBytes = 80'000U; direct.maximumStderrBytes = 20'000U;
    const auto escapedJob = take(durable.startProcess(direct, shellAuthority, context(55U)));
    const auto escapedResult = waitForDurable(durable, escapedJob.jobId);
    require(escapedResult.state == Domain::ShellJobState::Completed && escapedResult.result &&
                escapedResult.result->stdoutUtf8.size() == 80'000U && escapedResult.result->stderrUtf8.size() == 20'000U &&
                std::filesystem::file_size(std::filesystem::path{std::u8string{
                    reinterpret_cast<const char8_t*>(escapedResult.receiptPath.data()), escapedResult.receiptPath.size()}}) > 512U * 1024U,
            "The maximum escaped stdout/stderr capture could not be recovered from its bounded receipt");
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command",
        "$bytes=[byte[]](0,65,66,67,255); [Console]::OpenStandardOutput().Write($bytes,0,5); "
        "[Console]::OpenStandardError().Write($bytes,0,5)"};
    const auto binaryJob = take(durable.startProcess(direct, shellAuthority, context(56U)));
    const auto binaryResult = waitForDurable(durable, binaryJob.jobId);
    require(binaryResult.state == Domain::ShellJobState::Completed && binaryResult.result &&
                binaryResult.result->exitCode == 0 && !binaryResult.logHash.empty(),
            "Binary stdout/stderr did not produce a verified final receipt");
    for (const bool stderrStream : {false, true}) {
        const auto binaryPage = take(durable.readJobLog(binaryJob.jobId, stderrStream, 0U, 0U,
            shellAuthority, context(57U)));
        require(binaryPage.text == "?ABC?" && binaryPage.textLossy && binaryPage.offset == 0U &&
                    binaryPage.nextOffset == 5U && binaryPage.totalBytes == 5U && !binaryPage.hasMore,
                "Binary log text was not NUL-free with honest loss/byte-offset metadata");
        const auto binaryPath = std::filesystem::path{std::u8string{
            reinterpret_cast<const char8_t*>(binaryPage.path.data()), binaryPage.path.size()}};
        std::ifstream rawInput{binaryPath, std::ios::binary};
        std::string raw(5U, '\0');
        rawInput.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        require(rawInput.gcount() == 5 && raw.front() == '\0' &&
                    static_cast<unsigned char>(raw.back()) == 255U,
                "Text-safe log projection modified the raw durable evidence bytes");
    }
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command",
        "$bytes=[byte[]]::new(40000); $bytes[0]=65; $bytes[39999]=66; "
        "for($i=1;$i -lt 39999;$i++){$bytes[$i]=128}; "
        "[Console]::OpenStandardOutput().Write($bytes,0,40000); "
        "$bytes[0]=128; $bytes[39999]=128; [Console]::OpenStandardError().Write($bytes,0,40000)"};
    const auto continuationJob = take(durable.startProcess(direct, shellAuthority, context(58U)));
    const auto continuationResult = waitForDurable(durable, continuationJob.jobId);
    require(continuationResult.state == Domain::ShellJobState::Completed && continuationResult.result &&
                continuationResult.result->exitCode == 0,
            "Malformed UTF-8 log fixture did not complete");
    const auto continuationPage = take(durable.readJobLog(continuationJob.jobId, false, 0U, 0U,
        shellAuthority, context(59U)));
    require(continuationPage.text.size() == 32U * 1024U && continuationPage.text.front() == 'A' &&
                continuationPage.nextOffset == 32U * 1024U && continuationPage.hasMore && continuationPage.textLossy,
            "A long invalid UTF-8 continuation run stalled its log byte cursor");
    const auto continuationTail = take(durable.readJobLog(continuationJob.jobId, false,
        continuationPage.nextOffset, 0U, shellAuthority, context(60U)));
    require(continuationTail.text == "B" && continuationTail.nextOffset == 40'000U &&
                !continuationTail.hasMore && continuationTail.textLossy,
            "Skipped continuation bytes did not advance with honest loss metadata");
    const auto onlyContinuation = take(durable.readJobLog(continuationJob.jobId, true, 0U, 0U,
        shellAuthority, context(61U)));
    require(onlyContinuation.text.empty() && onlyContinuation.offset == 40'000U &&
                onlyContinuation.nextOffset == 40'000U && !onlyContinuation.hasMore && onlyContinuation.textLossy,
            "An all-continuation log did not finish with honest loss metadata");
    direct.arguments = {"-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 30"};
    const auto liveOwned = take(durable.startProcess(direct, shellAuthority, context(48U)));
    NativeTools::WindowsShellService otherHost{powerShellExecutable, supervisor, jobRoot};
    const auto foreignAdoption = otherHost.adoptJob(liveOwned.jobId, shellAuthority, context(49U));
    require(!foreignAdoption && foreignAdoption.error().code == Domain::ErrorCodes::OwnershipConflict,
            "receipt adoption accepted a live process without its owning job host");
    static_cast<void>(take(durable.cancelJob(liveOwned.jobId, shellAuthority, context(50U))));
    static_cast<void>(waitForDurable(durable, liveOwned.jobId));
    durable.shutdown();
    const auto restored = take(otherHost.adoptJob(directJob.jobId, shellAuthority, context(51U)));
    require(restored.state == Domain::ShellJobState::Completed && restored.result && restored.memoryAttached &&
                restored.logHash == durableResult.logHash && restored.processId == directJob.processId,
            "completed process evidence did not survive replacement of its shell service");
    const auto restoredList = take(otherHost.listJobs(shellAuthority, context(52U)));
    require(restoredList.size() == 7U, "durable process list did not restore completed receipts");
    const auto tamperPath = std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(restored.stdoutPath.data()), restored.stdoutPath.size()}};
    { std::ofstream output{tamperPath, std::ios::binary | std::ios::app}; output << "tampered"; }
    const auto tampered = otherHost.getJob(directJob.jobId, shellAuthority, context(53U));
    require(!tampered && tampered.error().code == Domain::ErrorCodes::IntegrityFailure,
            "restored process evidence did not detect log tampering");
    const auto receiptPath = std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(attachmentResult.receiptPath.data()), attachmentResult.receiptPath.size()}};
    { std::ofstream output{receiptPath, std::ios::binary | std::ios::app}; output << "tampered"; }
    const auto tamperedReceipt = otherHost.getJob(attachmentJob.jobId, shellAuthority, context(54U));
    require(!tamperedReceipt && tamperedReceipt.error().code == Domain::ErrorCodes::IntegrityFailure,
            "restored process evidence did not detect receipt tampering");
    otherHost.shutdown();

    shell.shutdown();

    const auto readyFile = workspace.path() / L"active-shell.ready";
    auto readyPath = pathText(readyFile).value();
    std::size_t quote{};
    while ((quote = readyPath.find('\'', quote)) != std::string::npos) {
        readyPath.insert(quote, 1U, '\'');
        quote += 2U;
    }
    auto activeShell = std::make_unique<NativeTools::WindowsShellService>(
        powerShellExecutable, supervisor);
    auto* const activeShellView = activeShell.get();
    Domain::ProcessRequest activeRequest{powerShellExecutable};
    activeRequest.arguments = {
        "Set-Content -LiteralPath '" + readyPath +
        "' -Value ready; Start-Sleep -Seconds 30"};
    activeRequest.workingDirectory = workspacePath;
    activeRequest.timeout = 60s;
    activeRequest.maximumStdoutBytes = 1'024U;
    activeRequest.maximumStderrBytes = 1'024U;
    const auto activeContext = context(18U);
    std::optional<Domain::Result<Domain::ProcessResult>> activeResult;
    std::jthread activeRun{[&] {
        activeResult.emplace(activeShellView->execute(
            activeRequest, shellAuthority, activeContext));
    }};
    const auto readyDeadline = std::chrono::steady_clock::now() + 10s;
    while (!std::filesystem::exists(readyFile) &&
           std::chrono::steady_clock::now() < readyDeadline) {
        std::this_thread::sleep_for(10ms);
    }
    require(std::filesystem::exists(readyFile),
            "active PowerShell fixture did not signal readiness");
    activeShell.reset();
    activeRun.join();
    require(activeResult.has_value() && activeResult.value() &&
                activeResult->value().cancelled,
            "active shell destruction did not cancel safely");

    supervisor->shutdown();
}

} // namespace

int main(const int argc, const char* const argv[])
{
    try {
        if (argc == 8 && std::string_view{argv[1]} == "--job-admission-child")
            return admissionChild(argv);
        require(argc == 3,
                "expected absolute Git and PowerShell executable paths");
        exerciseGitAndShell(
            std::filesystem::path{argv[1]},
            std::filesystem::path{argv[2]});
        std::cout << "PASS native_tools.git_shell_windows_integration\n";
        std::cout << "SUMMARY passed=1 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
