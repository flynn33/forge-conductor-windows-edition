#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
    const auto powerShellRoot = pathText(powerShellPath.parent_path());

    Infrastructure::SystemClock clock;
    const auto budgets = Domain::budgetsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    auto diagnostics = std::make_shared<Infrastructure::WindowsRuntimeDiagnostics>(
        clock, budgets);
    auto supervisor = std::make_shared<Infrastructure::WindowsProcessSupervisor>(
        budgets, diagnostics);

    const auto clientId =
        parse<Domain::ClientId>("p13-native-tool-integration-client");
    const auto gitProject = parse<Domain::ProjectId>(
        "61616161-6161-4161-8161-616161616161");
    const auto gitAuthority = makeAuthority(
        parse<Domain::AuthorityId>(
            "71717171-7171-4171-8171-717171717171"),
        gitProject,
        clientId,
        {workspacePath, gitRoot},
        context(1U));

    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitAuthority,
        {"init", "--quiet"}, 2U));
    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitAuthority,
        {"config", "user.name", "Forge Conductor Test"}, 3U));
    static_cast<void>(runSetupGit(
        *supervisor, gitExecutable, workspacePath, gitAuthority,
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
        gitExecutable, gitAuthority, supervisor};

    const auto status = take(git.status(repositoryRead, 8'192U, context(8U)));
    require(status.exitCode == 0 &&
                status.stdoutUtf8.find("tracked.txt") != std::string::npos,
            "Git status omitted the untracked fixture");
    const auto added = take(git.add(
        repositoryWrite,
        std::span<const Contracts::AuthorizedPath>{&fileRead, 1U},
        context(9U)));
    require(added.exitCode == 0,
            "Git add failed through the production adapter");
    const std::vector<std::string> cached{"--cached"};
    const auto diff = take(git.diff(
        repositoryRead, cached, 16'384U, context(10U)));
    require(diff.exitCode == 0 &&
                diff.stdoutUtf8.find("native Git adapter integration") !=
                    std::string::npos,
            "Git cached diff omitted the fixture content");
    const auto committed = take(git.commit(
        repositoryWrite, "P13 native Git integration", context(11U)));
    require(committed.exitCode == 0,
            "Git commit returned a nonzero process outcome");
    const auto log = take(git.log(
        repositoryRead, 1U, 8'192U, context(12U)));
    require(log.exitCode == 0 &&
                log.stdoutUtf8.find("P13 native Git integration") !=
                    std::string::npos,
            "Git log omitted the committed message");
    const auto nonzero = take(git.commit(
        repositoryWrite, "P13 no-op commit", context(13U)));
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
        {workspacePath, powerShellRoot},
        context(14U));
    NativeTools::WindowsShellService shell{
        powerShellExecutable, shellAuthority, supervisor};

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

    shell.shutdown();

    const auto readyFile = workspace.path() / L"active-shell.ready";
    auto readyPath = pathText(readyFile).value();
    std::size_t quote{};
    while ((quote = readyPath.find('\'', quote)) != std::string::npos) {
        readyPath.insert(quote, 1U, '\'');
        quote += 2U;
    }
    auto activeShell = std::make_unique<NativeTools::WindowsShellService>(
        powerShellExecutable, shellAuthority, supervisor);
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
