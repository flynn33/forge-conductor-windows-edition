#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"

#include "NativeToolValidation.h"
#include "ShellJobStorage.h"
#include "CMakeTestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"

#include "ForgeConductor/Domain/Utf8.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <climits>
#include <chrono>
#include <mutex>
#include <thread>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

constexpr std::string_view Utf8OutputPrefix =
    "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
    "$OutputEncoding=[Console]::OutputEncoding;";

constexpr std::string_view StdinCommandLoader =
    "[Console]::InputEncoding=[System.Text.UTF8Encoding]::new($false);"
    "& ([scriptblock]::Create([Console]::In.ReadToEnd()))";
constexpr std::string_view CommandExitStatus =
    "\nif (!$?) {exit 1}";

constexpr std::size_t MaximumShellPathBytes = 4'000U;
constexpr wchar_t MachineEnvironmentKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
constexpr wchar_t UserEnvironmentKey[] = L"Environment";
constexpr wchar_t WindowsCurrentVersionKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion";

[[nodiscard]] bool asciiNameEquals(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        unsigned char lhs = static_cast<unsigned char>(left[index]);
        unsigned char rhs = static_cast<unsigned char>(right[index]);
        if (lhs >= 'A' && lhs <= 'Z') {
            lhs = static_cast<unsigned char>(lhs - 'A' + 'a');
        }
        if (rhs >= 'A' && rhs <= 'Z') {
            rhs = static_cast<unsigned char>(rhs - 'A' + 'a');
        }
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasEnvironmentName(
    const std::vector<Domain::EnvironmentVariable>& environment,
    const std::string_view name) noexcept
{
    return std::any_of(
        environment.begin(), environment.end(),
        [name](const Domain::EnvironmentVariable& variable) noexcept {
            return asciiNameEquals(variable.name, name);
        });
}

[[nodiscard]] std::optional<std::string> wideToUtf8(const std::wstring_view value)
{
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int inputLength = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
        converted.data(), required, nullptr, nullptr);
    if (written != required) {
        return std::nullopt;
    }
    return converted;
}

[[nodiscard]] std::optional<std::wstring> registryString(
    const HKEY root,
    const wchar_t* const subkey,
    const wchar_t* const name)
{
    DWORD size = 0U;
    const LSTATUS measured = ::RegGetValueW(
        root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, nullptr, &size);
    if (measured != ERROR_SUCCESS || size < sizeof(wchar_t) || size > 32U * 1024U) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD bytes = size;
    const LSTATUS read = ::RegGetValueW(
        root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, value.data(), &bytes);
    if (read != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::wstring windowsDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT written = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    if (written == 0U || written >= MAX_PATH) {
        return L"C:\\Windows";
    }
    return std::wstring{buffer, written};
}

[[nodiscard]] std::wstring systemDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT written = ::GetSystemDirectoryW(buffer, MAX_PATH);
    if (written == 0U || written >= MAX_PATH) {
        return L"C:\\Windows\\System32";
    }
    return std::wstring{buffer, written};
}

[[nodiscard]] std::wstring programFilesDirectory()
{
    if (const auto configured = registryString(
            HKEY_LOCAL_MACHINE, WindowsCurrentVersionKey, L"ProgramFilesDir")) {
        return *configured;
    }
    return L"C:\\Program Files";
}

[[nodiscard]] bool isDirectory(const std::wstring_view path)
{
    if (path.empty() || path.size() > static_cast<std::size_t>(MAX_PATH) * 4U) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(std::wstring{path}.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

void appendUniqueDirectory(
    std::vector<std::wstring>& directories,
    std::wstring candidate)
{
    while (!candidate.empty() &&
           (candidate.back() == L'\\' || candidate.back() == L'/')) {
        candidate.pop_back();
    }
    if (candidate.empty() || !isDirectory(candidate)) {
        return;
    }
    const auto duplicate = std::any_of(
        directories.begin(), directories.end(),
        [&](const std::wstring& existing) noexcept {
            return existing.size() <= static_cast<std::size_t>(INT_MAX) &&
                candidate.size() <= static_cast<std::size_t>(INT_MAX) &&
                ::CompareStringOrdinal(
                    existing.data(), static_cast<int>(existing.size()),
                    candidate.data(), static_cast<int>(candidate.size()),
                    TRUE) == CSTR_EQUAL;
        });
    if (!duplicate) {
        directories.push_back(std::move(candidate));
    }
}

void appendPathList(
    std::vector<std::wstring>& directories,
    const std::wstring_view pathList)
{
    std::size_t start = 0U;
    while (start <= pathList.size()) {
        const auto end = pathList.find(L';', start);
        auto entry = std::wstring{
            pathList.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start)};
        const auto first = entry.find_first_not_of(L" \t\"");
        if (first == std::wstring::npos) {
            entry.clear();
        } else {
            const auto last = entry.find_last_not_of(L" \t\"");
            entry = entry.substr(first, last - first + 1U);
        }
        appendUniqueDirectory(directories, std::move(entry));
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1U;
    }
}

[[nodiscard]] std::string shellSearchPath()
{
    std::vector<std::wstring> directories;
    const auto windows = windowsDirectory();
    const auto system = systemDirectory();
    const auto programFiles = programFilesDirectory();
    appendUniqueDirectory(directories, system);
    appendUniqueDirectory(directories, windows);
    appendUniqueDirectory(directories, system + L"\\Wbem");
    appendUniqueDirectory(directories, system + L"\\WindowsPowerShell\\v1.0");
    appendUniqueDirectory(directories, system + L"\\OpenSSH");
    appendUniqueDirectory(directories, programFiles + L"\\PowerShell\\7");
    appendUniqueDirectory(directories, programFiles + L"\\Git\\cmd");
    appendUniqueDirectory(directories, programFiles + L"\\Git\\bin");
    appendUniqueDirectory(directories, programFiles + L"\\Python312\\Scripts");
    appendUniqueDirectory(directories, programFiles + L"\\Python312");
    appendUniqueDirectory(directories, programFiles + L"\\nodejs");
    appendUniqueDirectory(directories, programFiles + L"\\CMake\\bin");
    appendUniqueDirectory(directories, programFiles + L"\\GitHub CLI");
    if (const auto machine = registryString(
            HKEY_LOCAL_MACHINE, MachineEnvironmentKey, L"Path")) {
        appendPathList(directories, *machine);
    }
    if (const auto user = registryString(HKEY_CURRENT_USER, UserEnvironmentKey, L"Path")) {
        appendPathList(directories, *user);
    }

    std::string joined;
    for (const auto& directory : directories) {
        const auto utf8 = wideToUtf8(directory);
        if (!utf8) {
            continue;
        }
        const std::size_t extra = utf8->size() + (joined.empty() ? 0U : 1U);
        if (joined.size() + extra > MaximumShellPathBytes) {
            break;
        }
        if (!joined.empty()) {
            joined.push_back(';');
        }
        joined.append(*utf8);
    }
    return joined;
}

[[nodiscard]] std::string shellPathExt()
{
    std::wstring value;
    if (const auto machine = registryString(
            HKEY_LOCAL_MACHINE, MachineEnvironmentKey, L"PATHEXT")) {
        value = *machine;
    }
    if (value.find(L".EXE") == std::wstring::npos &&
        value.find(L".exe") == std::wstring::npos) {
        value = L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";
    }
    auto utf8 = wideToUtf8(value);
    if (!utf8 || utf8->size() > MaximumShellPathBytes) {
        return ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";
    }
    return std::move(*utf8);
}

[[nodiscard]] std::optional<std::string> shellHostEnvironmentValue(
    const wchar_t* const name)
{
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U || required > Domain::MaximumProcessEnvironmentValueBytes + 1U) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0U || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    auto utf8 = wideToUtf8(value);
    if (!utf8 || utf8->size() > Domain::MaximumProcessEnvironmentValueBytes) {
        return std::nullopt;
    }
    return utf8;
}

void ensureShellToolchainEnvironment(
    std::vector<Domain::EnvironmentVariable>& environment)
{
    // The process supervisor inherits only SystemRoot, WINDIR, TEMP, and TMP.
    // Without PATH and PATHEXT, PowerShell cannot resolve git.exe, cmd.exe, or
    // pwsh.exe, so an explicit toolchain path is supplied here.
    if (!hasEnvironmentName(environment, "PATH")) {
        auto path = shellSearchPath();
        if (!path.empty()) {
            environment.push_back(Domain::EnvironmentVariable{"PATH", std::move(path)});
        }
    }
    if (!hasEnvironmentName(environment, "PATHEXT")) {
        environment.push_back(Domain::EnvironmentVariable{"PATHEXT", shellPathExt()});
    }
    if (!hasEnvironmentName(environment, "COMSPEC")) {
        const auto comspec = wideToUtf8(systemDirectory() + L"\\cmd.exe");
        if (comspec && !comspec->empty() && comspec->size() <= MaximumShellPathBytes) {
            environment.push_back(Domain::EnvironmentVariable{"COMSPEC", *comspec});
        }
    }
    constexpr std::pair<std::string_view, const wchar_t*> hostVariables[]{
        {"USERNAME", L"USERNAME"}, {"USERDOMAIN", L"USERDOMAIN"},
        {"USERPROFILE", L"USERPROFILE"}, {"APPDATA", L"APPDATA"},
        {"LOCALAPPDATA", L"LOCALAPPDATA"}, {"HOMEDRIVE", L"HOMEDRIVE"},
        {"HOMEPATH", L"HOMEPATH"}, {"ProgramFiles", L"ProgramFiles"},
        {"ProgramFiles(x86)", L"ProgramFiles(x86)"}, {"ProgramData", L"ProgramData"},
        {"SystemDrive", L"SystemDrive"}};
    for (const auto& [name, wideName] : hostVariables) {
        if (!hasEnvironmentName(environment, name)) {
            if (auto value = shellHostEnvironmentValue(wideName)) {
                environment.push_back({std::string{name}, std::move(*value)});
            }
        }
    }
    if (!hasEnvironmentName(environment, "PYTHONUTF8")) {
        environment.push_back({"PYTHONUTF8", "1"});
    }
    if (!hasEnvironmentName(environment, "PYTHONIOENCODING")) {
        environment.push_back({"PYTHONIOENCODING", "utf-8"});
    }
}

[[nodiscard]] Domain::Result<void> validateCommandEnvelope(
    const Domain::ProcessRequest& request,
    const Domain::PathText& exactPowerShellExecutable) noexcept
{
    try {
        if (request.executable != exactPowerShellExecutable) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request executable does not match the injected PowerShell path."));
        }
        if (request.arguments.size() != 1U || request.arguments.front().empty()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request must contain exactly one nonempty command string."));
        }
        const auto& command = request.arguments.front();
        if (command.size() > WindowsShellService::MaximumCommandBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The PowerShell command exceeds 65536 UTF-8 bytes."));
        }
        if (command.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(command)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The PowerShell command must be valid NUL-free UTF-8."));
        }
        if (!request.workingDirectory) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request requires an authorized working directory."));
        }
        if (request.timeout <= std::chrono::milliseconds::zero()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The PowerShell timeout must be positive."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The PowerShell command envelope could not be validated."));
    }
}

void enforceOutputBounds(
    Domain::ProcessResult& result,
    const std::size_t maximumStdoutBytes,
    const std::size_t maximumStderrBytes)
{
    const auto truncate = [](std::string& value, const std::size_t maximumBytes) {
        if (value.size() <= maximumBytes) {
            return false;
        }
        std::size_t boundary = maximumBytes;
        while (boundary > 0U && boundary < value.size() &&
               (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
            --boundary;
        }
        value.resize(boundary);
        return true;
    };
    result.stdoutTruncated =
        truncate(result.stdoutUtf8, maximumStdoutBytes) ||
        result.stdoutTruncated;
    result.stderrTruncated =
        truncate(result.stderrUtf8, maximumStderrBytes) ||
        result.stderrTruncated;
}

[[nodiscard]] Domain::Result<Domain::ProcessRequest> resolveProcessRequest(Domain::ProcessRequest request)
{
    try {
        const auto native = [](const std::string_view text) {
            return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
        };
        auto executable = native(request.executable.value());
        if (!executable.is_absolute()) {
            if (executable.has_parent_path()) {
                if (!request.workingDirectory) return Domain::Result<Domain::ProcessRequest>::failure(
                    Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Process cwd is required."));
                executable = std::filesystem::absolute(native(request.workingDirectory->value()) / executable);
            } else {
                auto searchPath = shellSearchPath();
                for (const auto& variable : request.environment)
                    if (asciiNameEquals(variable.name, "PATH")) searchPath = variable.value;
                std::wstring found(32'768U, L'\0');
                const auto search = native(searchPath).wstring();
                const auto count = ::SearchPathW(search.c_str(), executable.c_str(), L".exe",
                    static_cast<DWORD>(found.size()), found.data(), nullptr);
                if (count == 0U || count >= found.size()) return Domain::Result<Domain::ProcessRequest>::failure(
                    Domain::makeError(Domain::ErrorCodes::ProcessLaunchFailed,
                        "The process executable could not be resolved through the effective PATH."));
                found.resize(count); executable = std::filesystem::path{found};
            }
        }
        const auto encoded = executable.generic_u8string();
        auto parsed = Domain::PathText::create(std::string{reinterpret_cast<const char*>(encoded.data()), encoded.size()});
        if (!parsed) return Domain::Result<Domain::ProcessRequest>::failure(std::move(parsed).error());
        request.executable = std::move(parsed).value();
        return Domain::Result<Domain::ProcessRequest>::success(std::move(request));
    } catch (...) {
        return Domain::Result<Domain::ProcessRequest>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "The direct process request could not be resolved."));
    }
}

} // namespace

class WindowsShellService::Impl final {
public:
    struct Job final {
        Domain::ProjectId projectId;
        Domain::OperationId operationId;
        Domain::ShellJobSnapshot snapshot;
        Domain::MonotonicTimePoint startedAt;
        std::stop_source cancellation;
        std::jthread worker;
        std::shared_ptr<Detail::ShellJobStorage> storage;
    };

    [[nodiscard]] Domain::ShellJobSnapshot snapshotOf(const Job& job) const
    {
        auto result = job.snapshot;
        if (job.storage) {
            result.processId = job.storage->processId();
            result.processCreationTime = job.storage->creationTime();
        }
        result.processAlive = Detail::ShellJobStorage::observeProcessAlive(
            result.processId, result.processCreationTime);
        if (result.state == Domain::ShellJobState::Running) {
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - job.startedAt);
        }
        return result;
    }

    struct ActiveOperation final {
        Domain::OperationId operationId;
        std::shared_ptr<std::stop_source> cancellation;
        bool cancellationRequested{};
    };

    Impl(
        Domain::PathText powerShellExecutable,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
        std::optional<Domain::PathText> storageRoot)
        : powerShellExecutable{std::move(powerShellExecutable)},
          processSupervisor{std::move(processSupervisor)}, jobRoot{std::move(storageRoot)}
    {
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<std::stop_source>> admit(
        const Domain::OperationId& operationId) noexcept
    {
        try {
            auto cancellation = std::make_shared<std::stop_source>();
            std::scoped_lock lock{stateMutex};
            if (shutdownRequested) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The shell service is shutting down."));
            }
            if (std::find_if(
                    activeOperations.begin(), activeOperations.end(),
                    [&](const ActiveOperation& active) {
                        return active.operationId == operationId;
                    }) != activeOperations.end()) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The shell operation identifier is already active."));
            }
            if (activeOperations.size() >=
                Contracts::IProcessSupervisor::MaximumConcurrentOperations) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RateLimited,
                        "The shell operation concurrency bound has been reached.",
                        true));
            }
            activeOperations.push_back(ActiveOperation{
                operationId, cancellation, false});
            return Domain::Result<std::shared_ptr<std::stop_source>>::success(
                std::move(cancellation));
        } catch (...) {
            return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The shell operation could not be admitted."));
        }
    }

    [[nodiscard]] bool release(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::scoped_lock lock{stateMutex};
            const auto match = std::find_if(
                activeOperations.begin(), activeOperations.end(),
                [&](const ActiveOperation& active) {
                    return active.operationId == operationId;
                });
            if (match == activeOperations.end()) {
                return true;
            }
            const auto cancelled = shutdownRequested ||
                match->cancellationRequested ||
                match->cancellation->stop_requested();
            activeOperations.erase(match);
            return cancelled;
        } catch (...) {
            return true;
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        std::shared_ptr<std::stop_source> cancellation;
        try {
            {
                std::scoped_lock lock{stateMutex};
                const auto match = std::find_if(
                    activeOperations.begin(), activeOperations.end(),
                    [&](const ActiveOperation& active) {
                        return active.operationId == operationId;
                    });
                if (match != activeOperations.end()) {
                    match->cancellationRequested = true;
                    cancellation = match->cancellation;
                }
            }
        } catch (...) {
            return;
        }
        if (cancellation) {
            cancellation->request_stop();
        }
        if (cancellation && processSupervisor) {
            processSupervisor->cancel(operationId);
        }
    }

    void shutdown() noexcept
    {
        std::scoped_lock shutdownLock{shutdownMutex};
        std::vector<std::shared_ptr<Job>> trackedJobs;
        std::vector<ActiveOperation> operations;
        try {
            {
                std::scoped_lock lock{stateMutex};
                if (shutdownRequested) {
                    return;
                }
                shutdownRequested = true;
                for (auto& operation : activeOperations) {
                    operation.cancellationRequested = true;
                }
                operations = activeOperations;
                trackedJobs = jobs;
            }
            for (const auto& job : trackedJobs) {
                job->cancellation.request_stop();
            }
            for (const auto& operation : operations) {
                operation.cancellation->request_stop();
            }
            if (processSupervisor) {
                for (const auto& operation : operations) {
                    processSupervisor->cancel(operation.operationId);
                }
            }
            for (const auto& job : trackedJobs) {
                if (job->worker.joinable()) {
                    job->worker.join();
                }
            }
        } catch (...) {
        }
    }

    const Domain::PathText powerShellExecutable;
    const std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor;
    std::mutex stateMutex;
    std::mutex shutdownMutex;
    const std::optional<Domain::PathText> jobRoot;
    JobCompletionSink completionSink;
    std::vector<std::shared_ptr<Job>> jobs;
    std::vector<ActiveOperation> activeOperations;
    bool shutdownRequested{};
};

WindowsShellService::WindowsShellService(
    Domain::PathText powerShellExecutable,
    std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
    std::optional<Domain::PathText> jobRoot)
    : implementation_{std::make_shared<Impl>(
          std::move(powerShellExecutable),
          std::move(processSupervisor), std::move(jobRoot))}
{
}

WindowsShellService::~WindowsShellService()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<Domain::ProcessResult> WindowsShellService::execute(
    const Domain::ProcessRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    return executeInternal(implementation, request, authority, context, false);
}

Domain::Result<Domain::ProcessResult> WindowsShellService::executeInternal(
    const std::shared_ptr<Impl>& implementation,
    const Domain::ProcessRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context,
    const bool managedJob,
    const bool directProcess) noexcept
{
    if (request.managedJob != managedJob) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Managed process execution requires a tracked shell job."));
    }
    if (!implementation) {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The PowerShell service is no longer available."));
    }
    bool admitted{};
    try {
        auto active = Detail::checkContext(context, "Shell operation");
        if (!active) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(active).error());
        }
        if (!authority.shellEnabled()) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ShellDisabled,
                    "PowerShell execution is disabled by workspace authority."));
        }
        if (!Detail::containsAccess(
                authority.grants(), Domain::FileAccess::Execute) ||
            Detail::containsAccess(
                authority.denials(), Domain::FileAccess::Execute)) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "Workspace authority does not grant PowerShell execution."));
        }
        if (!implementation->processSupervisor) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The shell service requires a process supervisor owner."));
        }
        auto executable = Detail::executableParent(
            directProcess ? request.executable : implementation->powerShellExecutable, "Process");
        if (!executable) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(executable).error());
        }
        auto envelope = directProcess ? Domain::Result<void>::success() : validateCommandEnvelope(
            request, implementation->powerShellExecutable);
        if (directProcess && !request.workingDirectory) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "A process launch requires an authorized working directory."));
        }
        if (!envelope) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(envelope).error());
        }
        auto workingDirectory = Detail::validateWorkingDirectory(
            request.workingDirectory.value(), authority);
        if (!workingDirectory) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(workingDirectory).error());
        }
        auto privateAuthority = Detail::derivePrivateExecutionAuthority(
            authority, directProcess ? request.executable : implementation->powerShellExecutable, "Process");
        if (!privateAuthority) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(privateAuthority).error());
        }

        Domain::ProcessRequest normalized{
            directProcess ? request.executable : implementation->powerShellExecutable};
        if (directProcess) {
            normalized.arguments = request.arguments;
            normalized.stdinUtf8 = request.stdinUtf8;
        } else {
            // The supervisor's bounded stdin writer owns script delivery and EOF.
            // Keep argv fixed below the Windows command-line and argument limits.
            std::string loader{Utf8OutputPrefix};
            loader.append(StdinCommandLoader);
            normalized.arguments = {"-NoLogo", "-NoProfile", "-NonInteractive",
                                    "-Command", std::move(loader)};
            normalized.stdinUtf8 = request.arguments.front();
            // Match -Command's final statement status inside the script block.
            normalized.stdinUtf8.append(CommandExitStatus);
        }
        normalized.outputObserver = request.outputObserver;
        normalized.workingDirectory = request.workingDirectory;
        normalized.environment = request.environment;
        ensureShellToolchainEnvironment(normalized.environment);
        // Windows PowerShell cannot initialize from an entirely empty environment
        // (it fails with 0x8009001D in packaged desktop processes). The supervisor
        // inherits only its fixed safe allowlist: SystemRoot, WINDIR, TEMP, and TMP.
        // Toolchain paths, allowlisted user/profile values, and Python UTF-8
        // defaults are supplied explicitly; arbitrary host secrets are omitted.
        normalized.inheritEnvironment = true;
        normalized.managedJob = managedJob;
        normalized.timeout = (std::min)(request.timeout,
            managedJob ? MaximumJobTimeout : MaximumTimeout);
        normalized.maximumStdoutBytes =
            (std::min)(request.maximumStdoutBytes, MaximumOutputBytes);
        normalized.maximumStderrBytes =
            (std::min)(request.maximumStderrBytes, MaximumErrorBytes);

        auto admission = implementation->admit(context.operationId);
        if (!admission) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(admission).error());
        }
        auto operationCancellation = std::move(admission).value();
        admitted = true;
        std::stop_callback callerCancellationBridge{
            context.cancellation,
            [operationCancellation]() noexcept {
                operationCancellation->request_stop();
            }};
        const Domain::OperationContext supervisorContext{
            context.operationId,
            context.deadline,
            operationCancellation->get_token(),
            context.correlationId};
        auto outcome = implementation->processSupervisor->run(
            normalized, privateAuthority.value(), supervisorContext);
        const auto cancelledLocally =
            implementation->release(context.operationId);
        admitted = false;
        if (!outcome) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(outcome).error());
        }
        auto result = std::move(outcome).value();
        if (cancelledLocally && !result.cancelled) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The PowerShell operation was cancelled before completion."));
        }
        enforceOutputBounds(
            result,
            normalized.maximumStdoutBytes,
            normalized.maximumStderrBytes);
        return Domain::Result<Domain::ProcessResult>::success(std::move(result));
    } catch (...) {
        if (admitted) {
            static_cast<void>(implementation->release(context.operationId));
        }
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The PowerShell request could not be executed."));
    }
}

void WindowsShellService::setJobCompletionSink(JobCompletionSink sink)
{
    const auto implementation = implementation_;
    if (!implementation) return;
    std::scoped_lock lock{implementation->stateMutex};
    implementation->completionSink = std::move(sink);
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::startJob(
    const Domain::ProcessRequest& request, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    return startOwnedJob(request, authority, context, false);
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::startProcess(
    const Domain::ProcessRequest& request, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    auto resolved = resolveProcessRequest(request);
    if (!resolved) return Domain::Result<Domain::ShellJobSnapshot>::failure(std::move(resolved).error());
    return startOwnedJob(resolved.value(), authority, context, true);
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::startCMakeTestRun(
    const Domain::CMakeTestRequest& request, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    using Outcome = Domain::Result<Domain::ShellJobSnapshot>;
    try {
        auto valid = Detail::validateCMakeTestRequest(request, authority, context);
        if (!valid) return Outcome::failure(std::move(valid).error());
        if (!implementation_ || !implementation_->jobRoot) return Outcome::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, "Structured CMake/CTest requires durable process storage."));
        auto executable = Domain::PathText::create("ctest");
        if (!executable) return Outcome::failure(std::move(executable).error());
        Domain::ProcessRequest process{std::move(executable).value()};
        process.workingDirectory = request.buildDirectory; process.timeout = request.timeout;
        process.maximumStdoutBytes = 16U * 1024U; process.maximumStderrBytes = 4U * 1024U;
        process.arguments = {"--test-dir", request.buildDirectory.value(), "--output-on-failure", "--no-tests=error"};
        if (request.filter) { process.arguments.push_back("-R"); process.arguments.push_back(*request.filter); }
        if (request.configuration) { process.arguments.push_back("-C"); process.arguments.push_back(*request.configuration); }
        auto resolved = resolveProcessRequest(std::move(process));
        if (!resolved) return Outcome::failure(std::move(resolved).error());
        return startOwnedJob(resolved.value(), authority, context, true, request);
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "CMake/CTest job could not be admitted."));
    }
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::startOwnedJob(
    const Domain::ProcessRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context,
    const bool directProcess,
    std::optional<Domain::CMakeTestRequest> cmakeTest) noexcept
{
    using Outcome = Domain::Result<Domain::ShellJobSnapshot>;
    const auto implementation = implementation_;
    try {
        auto active = Detail::checkContext(context, "Shell job admission");
        if (!active) return Outcome::failure(std::move(active).error());
        if (!implementation || !implementation->processSupervisor) {
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,
                "The shell service is unavailable."));
        }
        if (!authority.shellEnabled()) {
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::ShellDisabled,
                "PowerShell execution is disabled by workspace authority."));
        }
        if (!Detail::containsAccess(authority.grants(), Domain::FileAccess::Execute) ||
            Detail::containsAccess(authority.denials(), Domain::FileAccess::Execute)) {
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,
                "Workspace authority does not grant PowerShell execution."));
        }
        if (request.managedJob || request.timeout > MaximumJobTimeout) {
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Tracked shell job timeout must be within 1 through 3600 seconds."));
        }
        if (!request.workingDirectory) return Outcome::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "A tracked process requires an authorized working directory."));
        auto envelope = directProcess ? Domain::Result<void>::success()
            : validateCommandEnvelope(request, implementation->powerShellExecutable);
        if (!envelope) return Outcome::failure(std::move(envelope).error());
        if (directProcess) {
            auto validationRequest = request;
            validationRequest.managedJob = true;
            auto valid = Domain::validateProcessRequest(validationRequest,
                Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB));
            if (!valid) return Outcome::failure(std::move(valid).error());
        }
        auto cwd = Detail::validateWorkingDirectory(*request.workingDirectory, authority);
        if (!cwd) return Outcome::failure(std::move(cwd).error());
        auto executable = Detail::executableParent(
            directProcess ? request.executable : implementation->powerShellExecutable, "Process");
        if (!executable) return Outcome::failure(std::move(executable).error());
        Infrastructure::Windows::WindowsUuidGenerator generator;
        auto generated = generator.next();
        if (!generated) return Outcome::failure(std::move(generated).error());
        Domain::OperationId operation{std::move(generated).value()};
        const auto now = std::chrono::steady_clock::now();
        Domain::ShellJobSnapshot initial{operation.value(), Domain::ShellJobState::Running,
            directProcess ? request.executable.value() : request.arguments.front(), request.workingDirectory->value(),
            static_cast<std::uint32_t>((request.timeout.count() + 999) / 1000),
            std::nullopt, std::nullopt, std::chrono::milliseconds::zero()};
        auto jobRequest = request;
        std::optional<Domain::ProcessRequest> buildPhase;
        if (cmakeTest) {
            const auto native = [](std::string_view value) { return std::filesystem::path{
                std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}}; };
            const auto report = native(implementation->jobRoot->value()) / authority.projectId().value() / (operation.value() + ".ctest.xml");
            if (std::filesystem::exists(report)) return Outcome::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict, "The CTest run report already exists; stale reports are not reused."));
            const auto encoded = report.generic_u8string();
            initial.cmakeTest.emplace();
            auto& metadata = *initial.cmakeTest;
            metadata.buildDirectory = cmakeTest->buildDirectory.value(); metadata.buildRequested = cmakeTest->build;
            metadata.target = cmakeTest->target; metadata.filter = cmakeTest->filter; metadata.configuration = cmakeTest->configuration;
            metadata.reportPath = {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
            jobRequest.arguments.push_back("--output-junit"); jobRequest.arguments.push_back(metadata.reportPath);
            if (cmakeTest->build) {
                auto executablePath = Domain::PathText::create("cmake");
                if (!executablePath) return Outcome::failure(std::move(executablePath).error());
                Domain::ProcessRequest phase{std::move(executablePath).value()};
                phase.workingDirectory = cmakeTest->buildDirectory; phase.timeout = request.timeout;
                phase.maximumStdoutBytes = 16U * 1024U; phase.maximumStderrBytes = 4U * 1024U;
                phase.arguments = {"--build", cmakeTest->buildDirectory.value()};
                if (cmakeTest->target) { phase.arguments.push_back("--target"); phase.arguments.push_back(*cmakeTest->target); }
                if (cmakeTest->configuration) { phase.arguments.push_back("--config"); phase.arguments.push_back(*cmakeTest->configuration); }
                auto resolved = resolveProcessRequest(std::move(phase));
                if (!resolved) return Outcome::failure(std::move(resolved).error());
                buildPhase = std::move(resolved).value();
            }
        }
        if (directProcess) initial.arguments = jobRequest.arguments;
        auto job = std::make_shared<Impl::Job>(Impl::Job{
            authority.projectId(), operation, initial, now, {}, {}, {}});
        std::shared_ptr<Impl::Job> retired;
        {
            std::scoped_lock lock{implementation->stateMutex};
            if (implementation->shutdownRequested) {
                return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,
                    "The shell service is shutting down."));
            }
            const auto activeJobs = std::count_if(implementation->jobs.begin(), implementation->jobs.end(),
                [](const auto& entry) { return entry->snapshot.state == Domain::ShellJobState::Running; });
            if (activeJobs >= static_cast<std::ptrdiff_t>(MaximumActiveJobs)) {
                return Outcome::failure(Domain::makeError(Domain::ErrorCodes::RateLimited,
                    "The two active shell jobs limit has been reached. Poll or cancel an existing job.", true));
            }
            if (implementation->jobRoot) {
                auto storage = Detail::ShellJobStorage::create(*implementation->jobRoot, authority.projectId(), initial, context);
                if (!storage) return Outcome::failure(std::move(storage).error());
                job->storage = std::move(storage).value();
                job->storage->persist(initial);
                job->snapshot = initial;
            }
            if (implementation->jobs.size() >= MaximumRetainedJobs) {
                const auto oldest = std::find_if(implementation->jobs.begin(), implementation->jobs.end(),
                    [](const auto& entry) { return entry->snapshot.state != Domain::ShellJobState::Running; });
                retired = *oldest;
                implementation->jobs.erase(oldest);
            }
            implementation->jobs.push_back(job);
            auto managedRequest = jobRequest;
            managedRequest.managedJob = true;
            if (job->storage) managedRequest.outputObserver = job->storage;
            try {
                job->worker = std::jthread{[implementation, job, managedRequest, authority, directProcess, buildPhase,
                    correlationId = context.correlationId]() noexcept {
                    try {
                        Domain::ShellJobSnapshot snapshot;
                        JobCompletionSink sink;
                        {
                            std::scoped_lock completionLock{implementation->stateMutex};
                            snapshot = job->snapshot;
                            sink = implementation->completionSink;
                        }
                        const Domain::OperationContext jobContext{job->operationId,
                            job->startedAt + managedRequest.timeout + (snapshot.cmakeTest ? std::chrono::seconds{0} : std::chrono::seconds{10}),
                            job->cancellation.get_token(), correlationId};
                        const auto phase = [&](Domain::ProcessRequest next) {
                            next.managedJob = true; next.outputObserver = managedRequest.outputObserver;
                            next.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(jobContext.deadline - std::chrono::steady_clock::now());
                            if (next.timeout.count() <= 0) return Domain::Result<Domain::ProcessResult>::failure(
                                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded, "CMake/CTest shared deadline expired before the next phase."));
                            return executeInternal(implementation, next, authority, jobContext, true, true);
                        };
                        const auto testPhase = [&] {
                            auto fresh = Detail::validateFreshCTestReport(*snapshot.cmakeTest);
                            if (!fresh) return Domain::Result<Domain::ProcessResult>::failure(std::move(fresh).error());
                            return phase(managedRequest);
                        };
                        auto outcome = snapshot.cmakeTest
                            ? (buildPhase ? phase(*buildPhase) : testPhase())
                            : executeInternal(implementation, managedRequest, authority, jobContext, true, directProcess);
                        if (snapshot.cmakeTest) {
                            auto& metadata = *snapshot.cmakeTest;
                            if (buildPhase && outcome) metadata.buildResult = outcome.value();
                            if (buildPhase && outcome && outcome.value().exitCode == 0 && outcome.value().terminationConfirmed &&
                                    !outcome.value().timedOut && !outcome.value().cancelled) {
                                outcome = testPhase();
                                if (outcome) metadata.testResult = outcome.value();
                            } else if (!buildPhase && outcome) metadata.testResult = outcome.value();
                            Detail::captureCTestReport(metadata, &jobContext);
                            if (metadata.counts) {
                                auto active = Detail::checkContext(jobContext, "CMake/CTest completion");
                                if (!active) { metadata.counts.reset(); metadata.reportError = std::move(active).error(); }
                            }
                        }
                        snapshot.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - job->startedAt);
                        if (!outcome) {
                            snapshot.error = std::move(outcome).error();
                            snapshot.state = snapshot.error->code == Domain::ErrorCodes::Cancelled
                                ? Domain::ShellJobState::Cancelled
                                : snapshot.error->code == Domain::ErrorCodes::DeadlineExceeded
                                    ? Domain::ShellJobState::TimedOut : Domain::ShellJobState::Failed;
                        } else {
                            snapshot.result = std::move(outcome).value();
                            const auto& result = *snapshot.result;
                            snapshot.state = result.cancelled ? Domain::ShellJobState::Cancelled
                                : result.timedOut ? Domain::ShellJobState::TimedOut
                                : result.exitCode == 0 && result.terminationConfirmed
                                    ? Domain::ShellJobState::Completed : Domain::ShellJobState::Failed;
                        }
                        if (snapshot.cmakeTest && snapshot.cmakeTest->reportError &&
                                (snapshot.state == Domain::ShellJobState::Completed || snapshot.cmakeTest->reportError->code == Domain::ErrorCodes::Cancelled ||
                                    snapshot.cmakeTest->reportError->code == Domain::ErrorCodes::DeadlineExceeded)) {
                            snapshot.error = snapshot.cmakeTest->reportError;
                            snapshot.state = snapshot.error->code == Domain::ErrorCodes::Cancelled ? Domain::ShellJobState::Cancelled
                                : snapshot.error->code == Domain::ErrorCodes::DeadlineExceeded ? Domain::ShellJobState::TimedOut : Domain::ShellJobState::Failed;
                        }
                        const auto persist = [&] {
                            if (!job->storage) return;
                            try {
                                if (snapshot.result) job->storage->captureFallback(*snapshot.result);
                                job->storage->persist(snapshot);
                            } catch (...) {
                                snapshot.error = Domain::makeError(Domain::ErrorCodes::StorageFull,
                                    "The final process receipt could not be published.");
                                snapshot.logHash.clear();
                            }
                        };
                        persist();
                        if (sink) {
                            try {
                                sink(job->projectId, snapshot);
                                snapshot.memoryAttached = true;
                            } catch (const std::exception& failure) {
                                snapshot.memoryAttachError = Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                    std::string{"The final process evidence could not be attached to project memory: "} + failure.what());
                            } catch (...) {
                                snapshot.memoryAttachError = Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                    "The final process evidence could not be attached to project memory.");
                            }
                            persist();
                        }
                        {
                            std::scoped_lock completionLock{implementation->stateMutex};
                            job->snapshot = std::move(snapshot);
                        }
                    } catch (...) {
                        try {
                            std::scoped_lock failureLock{implementation->stateMutex};
                            job->snapshot.state = Domain::ShellJobState::Failed;
                            job->snapshot.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - job->startedAt);
                            job->snapshot.error = Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                "The tracked shell job worker failed.");
                        } catch (...) {
                        }
                    }
                }};
            } catch (...) {
                if (job->storage) {
                    try {
                        auto failed = job->snapshot;
                        failed.state = Domain::ShellJobState::Failed;
                        failed.error = Domain::makeError(Domain::ErrorCodes::InternalFailure,
                            "The process job worker could not be created.");
                        job->storage->persist(failed);
                    } catch (...) {}
                }
                implementation->jobs.pop_back();
                throw;
            }
        }
        if (retired && retired->worker.joinable()) retired->worker.join();
        if (job->storage) {
            job->storage->waitForStarted(std::chrono::seconds{5});
            std::scoped_lock lock{implementation->stateMutex};
            return Outcome::success(implementation->snapshotOf(*job));
        }
        return Outcome::success(std::move(initial));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "The tracked shell job could not be started."));
    }
}

Domain::Result<Domain::CMakeTestRunStatus> WindowsShellService::getCMakeTestRun(
    const std::string_view id, const std::uint64_t offset, const std::size_t limit,
    const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) noexcept
{
    using Outcome = Domain::Result<Domain::CMakeTestRunStatus>;
    try {
        if (!Detail::containsAccess(authority.grants(), Domain::FileAccess::Read) || Detail::containsAccess(authority.denials(), Domain::FileAccess::Read))
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized, "CTest status requires read authority."));
        if (!limit || limit > Detail::MaximumCTestFailuresPerPage) return Outcome::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "CTest failure page must contain 1 through 32 cases."));
        auto snapshot = getJob(id, authority, context);
        if (!snapshot) return Outcome::failure(std::move(snapshot).error());
        auto active = Detail::checkContext(context, "CMake/CTest status");
        if (!active) return Outcome::failure(std::move(active).error());
        if (!snapshot.value().cmakeTest) return Outcome::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "The owned job is not a structured CMake/CTest run."));
        Domain::CMakeTestRunStatus result{std::move(snapshot).value(), {}, offset, offset, 0U, false};
        const auto& metadata = *result.job.cmakeTest;
        if (metadata.counts) {
            if (offset > metadata.counts->failed) return Outcome::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "CTest failure offset exceeds the report."));
            auto report = Detail::readCTestReport(metadata, offset, limit, &context);
            if (!report) return Outcome::failure(std::move(report).error());
            result.failures = std::move(report).value().failures;
            result.totalFailures = metadata.counts->failed;
            result.nextFailureOffset += result.failures.size();
            result.hasMore = result.nextFailureOffset < result.totalFailures;
        } else if (offset) return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
            "A CTest failure offset requires an available completed report."));
        return Outcome::success(std::move(result));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "CMake/CTest status could not be read."));
    }
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::getJob(
    const std::string_view jobId, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    using Outcome = Domain::Result<Domain::ShellJobSnapshot>;
    try {
        auto active = Detail::checkContext(context, "Shell job status");
        if (!active) return Outcome::failure(std::move(active).error());
        const auto implementation = implementation_;
        std::optional<Domain::ShellJobSnapshot> local;
        if (implementation) {
            std::scoped_lock lock{implementation->stateMutex};
            for (const auto& job : implementation->jobs) {
                if (job->snapshot.jobId == jobId) {
                    if (job->projectId != authority.projectId()) {
                        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,
                            "The shell job belongs to another project."));
                    }
                    local = implementation->snapshotOf(*job);
                    break;
                }
            }
        }
        if (local && (local->state == Domain::ShellJobState::Running || !implementation->jobRoot ||
                (local->error && local->logHash.empty()))) {
            return Outcome::success(std::move(*local));
        }
        if (implementation && implementation->jobRoot) {
            return Detail::ShellJobStorage::load(*implementation->jobRoot, authority.projectId(), jobId);
        }
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::RecordNotFound,
            "The shell job is unknown or its retained result has expired."));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "The tracked shell job status could not be read."));
    }
}

Domain::Result<std::vector<Domain::ShellJobSnapshot>> WindowsShellService::listJobs(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    using Outcome = Domain::Result<std::vector<Domain::ShellJobSnapshot>>;
    try {
        auto active = Detail::checkContext(context, "Shell job list");
        if (!active) return Outcome::failure(std::move(active).error());
        std::vector<Domain::ShellJobSnapshot> results;
        const auto implementation = implementation_;
        if (implementation) {
            std::scoped_lock lock{implementation->stateMutex};
            for (const auto& job : implementation->jobs) {
                if (job->projectId == authority.projectId()) {
                    results.push_back(implementation->snapshotOf(*job));
                }
            }
        }
        if (implementation && implementation->jobRoot) {
            auto persisted = Detail::ShellJobStorage::list(*implementation->jobRoot, authority.projectId());
            if (!persisted) return Outcome::failure(std::move(persisted).error());
            for (auto& snapshot : persisted.value()) {
                if (std::none_of(results.begin(), results.end(), [&](const auto& entry) { return entry.jobId == snapshot.jobId; })) {
                    results.push_back(std::move(snapshot));
                }
            }
        }
        return Outcome::success(std::move(results));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "The tracked shell jobs could not be listed."));
    }
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::cancelJob(
    const std::string_view jobId, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    using Outcome = Domain::Result<Domain::ShellJobSnapshot>;
    try {
        auto active = Detail::checkContext(context, "Shell job cancellation");
        if (!active) return Outcome::failure(std::move(active).error());
        std::shared_ptr<Impl::Job> selected;
        const auto implementation = implementation_;
        if (implementation) {
            std::scoped_lock lock{implementation->stateMutex};
            for (const auto& job : implementation->jobs) {
                if (job->snapshot.jobId == jobId) {
                    if (job->projectId != authority.projectId()) {
                        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,
                            "The shell job belongs to another project."));
                    }
                    if (job->snapshot.state == Domain::ShellJobState::Running) selected = job;
                    break;
                }
            }
        }
        if (selected) {
            selected->cancellation.request_stop();
            implementation->cancel(selected->operationId);
        }
        return getJob(jobId, authority, context);
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "The tracked shell job could not be cancelled."));
    }
}

Domain::Result<Domain::ShellJobSnapshot> WindowsShellService::adoptJob(
    const std::string_view id, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    return getJob(id, authority, context);
}

Domain::Result<Domain::ShellJobLogPage> WindowsShellService::readJobLog(
    const std::string_view id, const bool stderrStream, const std::optional<std::uint64_t> offset,
    const std::size_t tailLines, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    auto snapshot = getJob(id, authority, context);
    if (!snapshot) return Domain::Result<Domain::ShellJobLogPage>::failure(std::move(snapshot).error());
    return Detail::ShellJobStorage::read(snapshot.value(), stderrStream, offset, tailLines);
}

void WindowsShellService::cancel(
    const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancel(operationId);
    }
}

void WindowsShellService::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::NativeTools::Windows
