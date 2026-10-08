#include "McpServeCompositionRoot.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAlphaManagerProfile.h"


#include "ForgeConductor/Application/AgentCatalog.h"
#include "ForgeConductor/Application/AgentSessionService.h"
#include "ForgeConductor/Application/ClientPresenceLifecycle.h"
#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "ForgeConductor/Application/LegacyContextContinuityService.h"
#include "ForgeConductor/Application/LegacyMemoryService.h"
#include "ForgeConductor/Application/ProjectMemoryRepositoryCache.h"
#include "ForgeConductor/Application/ProjectMemoryService.h"
#include "ForgeConductor/Application/ProjectPolicyService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsPolicySourceReader.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsGitHubReadService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsSystemInspection.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Mcp/McpClientWorkspaceContext.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpInvocationGuard.h"
#include "ForgeConductor/Mcp/McpServer.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include "ForgeConductor/Mcp/McpToolPackAdapter.h"
#include "ForgeConductor/Mcp/McpToolRouter.h"
#include "ForgeConductor/Mcp/WindowsStdioMcpTransport.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsPathGlobService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsPdfService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsArtifactDocumentService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsDesktopArtifactService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsWebAccessService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsEvidenceService.h"
#include <nlohmann/json.hpp>
#include "ForgeConductor/NativeTools/Windows/WindowsTextSearchService.h"
#include "ForgeConductor/Persistence/Windows/PersistenceWindows.h"
#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Hosts::Cli {
namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace Mcp = ForgeConductor::Mcp;
namespace NativeToolsWindows = ForgeConductor::NativeTools::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace NativeSessionHost = ForgeConductor::SessionHost;

constexpr std::chrono::seconds StartupTimeout{30};
constexpr std::chrono::seconds ShutdownTimeout{10};
constexpr auto ProductVersion = Domain::ProductVersion;
constexpr std::string_view RuntimeName{"forge-conductor-windows-stdio"};
constexpr std::size_t MaximumEnvironmentValueCharacters = 32U * 1024U;

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void requireSuccess(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

[[nodiscard]] Domain::PathText pathText(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::Result<std::string> strictWideToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path could not be converted to UTF-8."));
        }
        const auto inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path is not valid UTF-16."));
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required, nullptr, nullptr) != required) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A Windows path conversion was incomplete."));
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows path conversion failed safely."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToWide(
    const std::string_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path could not be converted from UTF-8."));
        }
        const auto inputLength = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0);
        if (required <= 0) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path is not valid UTF-8."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required) != required) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A Windows path conversion was incomplete."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows path conversion failed safely."));
    }
}

[[nodiscard]] std::optional<std::string> environmentValue(
    const wchar_t* const name)
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U) {
        if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::string{};
    }
    if (required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{"An MCP environment value exceeds its bound."};
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    ::SetLastError(ERROR_SUCCESS);
    const DWORD written = ::GetEnvironmentVariableW(
        name, buffer.data(), required);
    const DWORD readError = ::GetLastError();
    if (written >= required || (written == 0U && readError != ERROR_SUCCESS)) {
        throw std::runtime_error{"An MCP environment value could not be read."};
    }
    if (written == 0U) {
        return std::string{};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return take(strictWideToUtf8(buffer));
}

[[nodiscard]] Domain::McpRole configuredRole()
{
    const auto configured = environmentValue(L"FORGE_MCP_ROLE");
    if (!configured || configured->empty() || *configured == "primary") {
        return Domain::McpRole::Primary;
    }
    if (*configured == "fallback") {
        return Domain::McpRole::Fallback;
    }
    if (*configured == "clu") {
        return Domain::McpRole::Clu;
    }
    throw std::runtime_error{
        "invalid_request: FORGE_MCP_ROLE must be primary, fallback, or clu."};
}

[[nodiscard]] Domain::PathText currentDirectory()
{
    const DWORD required = ::GetCurrentDirectoryW(0U, nullptr);
    if (required == 0U || required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{
            "internal_failure: The startup working directory could not be resolved."};
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetCurrentDirectoryW(required, buffer.data());
    if (written == 0U || written >= required) {
        throw std::runtime_error{
            "internal_failure: The startup working directory could not be read."};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return pathText(take(strictWideToUtf8(buffer)));
}

[[nodiscard]] Domain::PathText discoverExecutable(const wchar_t* const name)
{
    const DWORD required = ::SearchPathW(nullptr, name, nullptr, 0U, nullptr, nullptr);
    if (required == 0U || required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{
            "host_capability_unavailable: A required native executable was not found."};
    }
    std::wstring buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::SearchPathW(
        nullptr, name, nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (written == 0U || written >= buffer.size()) {
        throw std::runtime_error{
            "host_capability_unavailable: A required native executable path could not be resolved."};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return pathText(take(strictWideToUtf8(buffer)));
}

[[nodiscard]] bool isSingleLinkRegularExecutable(
    const std::filesystem::path& candidate) noexcept
{
    const HANDLE file = ::CreateFileW(
        candidate.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    const bool valid =
        ::GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE &&
        ::GetFileInformationByHandleEx(
            file, FileStandardInfo, &standard, sizeof(standard)) != FALSE &&
        (attributes.FileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U &&
        standard.DeletePending == FALSE && standard.NumberOfLinks == 1U;
    ::CloseHandle(file);
    return valid;
}

[[nodiscard]] Domain::PathText discoverGitExecutable()
{
    const auto searched = discoverExecutable(L"git.exe");
    const auto searchedWide = take(strictUtf8ToWide(searched.value()));
    const std::filesystem::path searchedPath{searchedWide};
    if (isSingleLinkRegularExecutable(searchedPath)) return searched;

    // Git for Windows may hard-link cmd\git.exe to git-lfs.exe. Its adjacent
    // bin\git.exe is the supported command entry point with a unique file
    // identity, which satisfies the process supervisor's launch invariant.
    if (_wcsicmp(
            searchedPath.parent_path().filename().c_str(), L"cmd") == 0) {
        const auto candidate = searchedPath.parent_path().parent_path() /
            L"bin" / L"git.exe";
        if (isSingleLinkRegularExecutable(candidate)) {
            return pathText(take(strictWideToUtf8(candidate.wstring())));
        }
    }
    return searched;
}

[[nodiscard]] std::optional<Domain::PathText> tryDiscoverExecutable(
    const wchar_t* const name)
{
    const DWORD required = ::SearchPathW(nullptr, name, nullptr, 0U, nullptr, nullptr);
    if (required == 0U || required > MaximumEnvironmentValueCharacters) {
        return std::nullopt;
    }
    std::wstring buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::SearchPathW(
        nullptr, name, nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (written == 0U || written >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(static_cast<std::size_t>(written));
    auto converted = strictWideToUtf8(buffer);
    if (!converted) {
        return std::nullopt;
    }
    auto text = Domain::PathText::create(converted.value());
    if (!text) {
        return std::nullopt;
    }
    const auto wide = strictUtf8ToWide(text.value().value());
    if (!wide || !isSingleLinkRegularExecutable(std::filesystem::path{wide.value()})) {
        return std::nullopt;
    }
    return std::move(text).value();
}

[[nodiscard]] std::optional<std::wstring> installedProgramFilesDirectory()
{
    DWORD size = 0U;
    const LSTATUS measured = ::RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"ProgramFilesDir",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, nullptr, &size);
    if (measured != ERROR_SUCCESS || size < sizeof(wchar_t) || size > 1024U) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD bytes = size;
    const LSTATUS read = ::RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"ProgramFilesDir",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, value.data(), &bytes);
    if (read != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// PowerShell 7 (pwsh.exe) is the host shell. Windows PowerShell 5.1 remains
// the fallback when pwsh is not installed as a regular executable.
[[nodiscard]] Domain::PathText discoverPowerShellExecutable()
{
    if (auto powerShell7 = tryDiscoverExecutable(L"pwsh.exe")) {
        return std::move(*powerShell7);
    }
    if (const auto programFiles = installedProgramFilesDirectory()) {
        const auto candidate = std::filesystem::path{*programFiles} /
            L"PowerShell" / L"7" / L"pwsh.exe";
        if (isSingleLinkRegularExecutable(candidate)) {
            return pathText(take(strictWideToUtf8(candidate.wstring())));
        }
    }
    return discoverExecutable(L"powershell.exe");
}

[[nodiscard]] Domain::PathText childPath(
    const Domain::PathText& root,
    const std::string_view relative)
{
    if (relative.empty() || relative.starts_with('/') || relative.starts_with('\\')) {
        throw std::runtime_error{"invalid_request: An app-owned relative path is invalid."};
    }
    std::string value = root.value();
    if (!value.ends_with('/') && !value.ends_with('\\')) {
        value.push_back('\\');
    }
    value.append(relative);
    return pathText(value);
}

void ensureDirectory(const Domain::PathText& directory)
{
    const auto wide = take(strictUtf8ToWide(directory.value()));
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path{wide}, error);
    if (error) {
        throw std::runtime_error{
            "internal_failure: An app-owned directory could not be created."};
    }
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        throw std::runtime_error{
            "path_outside_authority: An app-owned root is not a regular directory."};
    }
}

[[nodiscard]] Domain::ResourceProfile resourceProfile()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status) != FALSE
        ? Domain::selectResourceProfile(status.ullTotalPhys)
        : Domain::ResourceProfile::Standard16GiB;
}

[[nodiscard]] Domain::Uuid nextUuid(Contracts::IUuidGenerator& generator)
{
    return take(generator.next());
}

[[nodiscard]] Domain::OperationContext makeContext(
    Contracts::IUuidGenerator& generator,
    const Contracts::IClock& clock,
    const std::chrono::seconds timeout,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        Domain::OperationId{nextUuid(generator)},
        clock.monotonicNow() + timeout,
        {},
        take(Domain::CorrelationId::parse(correlation))};
}

[[nodiscard]] InfrastructureWindows::WindowsWorkspaceAuthorityPolicy authorityPolicy(
    Domain::AuthorityId authorityId,
    Domain::ProjectId projectId,
    Domain::ClientId clientId,
    std::vector<Domain::PathText> roots,
    const Domain::FileAccess intent,
    std::vector<Domain::FileAccess> grants,
    std::vector<Domain::FileAccess> denials,
    const bool shellEnabled)
{
    return InfrastructureWindows::WindowsWorkspaceAuthorityPolicy{
        std::move(authorityId), std::move(projectId), std::move(clientId),
        std::move(roots), intent, std::move(grants), std::move(denials),
        shellEnabled, 1U};
}

[[nodiscard]] Contracts::AuthorizedPath authorizePath(
    Contracts::IWorkspaceAuthority& issuer,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& target,
    const Domain::PathText& root,
    const Domain::FileAccess access,
    const Domain::OperationContext& context)
{
    return take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{target, root, access, false},
        context));
}

[[nodiscard]] bool sameWindowsPath(const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring_view value)
{
    std::wstring quoted{L"\""};
    std::size_t backslashes{};
    for (const auto character : value) {
        if (character == L'\\') { ++backslashes; continue; }
        quoted.append(backslashes * (character == L'\"' ? 2U : 1U) +
            (character == L'\"' ? 1U : 0U), L'\\');
        quoted.push_back(character);
        backslashes = 0U;
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

[[nodiscard]] std::optional<std::filesystem::path> siblingManagerExecutable()
{
    std::array<wchar_t, 32'768U> module{};
    const auto length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return std::nullopt;
    return std::filesystem::path{std::wstring{module.data(), length}}.parent_path() /
        L"ForgeConductor.Manager.exe";
}

[[nodiscard]] Domain::Result<void> validateDurableManagerStatus(const Domain::ManagerStatus& status,
    const Domain::PathText& home, const std::filesystem::path& executable)
{
    const auto unavailable = [](const char* message) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, message));
    };
    if (!status.isManager || status.processId == 0U)
        return unavailable("The authenticated endpoint is not a live durable Manager.");
    if (status.version != ProductVersion)
        return unavailable("The durable Manager version differs from this connector; use matching package binaries.");
    auto expectedHome = strictUtf8ToWide(home.value());
    auto servingHome = strictUtf8ToWide(status.home.value());
    if (!expectedHome || !servingHome || !sameWindowsPath(servingHome.value(), expectedHome.value()))
        return unavailable("The durable Manager home differs from this connector profile.");
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, status.processId);
    if (!process) return unavailable("The durable Manager serving process could not be verified.");
    std::array<wchar_t, 32'768U> image{};
    DWORD characters = static_cast<DWORD>(image.size());
    const bool matching = ::QueryFullProcessImageNameW(process, 0U, image.data(), &characters) != FALSE &&
        sameWindowsPath(std::wstring_view{image.data(), characters}, executable.native());
    ::CloseHandle(process);
    if (!matching) return unavailable("The authenticated Manager is serving from a different executable package.");
    return Domain::Result<void>::success();
}

// A broker owns its own lifetime. Startup never replaces an existing lease,
// inherits MCP stdio, or connects across profile/version/package boundaries.
[[nodiscard]] std::unique_ptr<InfrastructureWindows::WindowsManagerNamedPipeClient>
ensureDurableManager(const Domain::PathText& home, const Domain::OperationContext& parent,
    const std::shared_ptr<InfrastructureWindows::SystemClock>& clock,
    std::string& startupError) noexcept
{
    try {
        struct StartupProcess final {
            HANDLE handle{};
            ~StartupProcess() noexcept
            {
                if (handle) static_cast<void>(::CloseHandle(handle));
            }
        } startupProcess;
        startupError = "The matching durable Manager could not be started or authenticated.";
        auto identity = InfrastructureWindows::WindowsCurrentUserIdentity::load();
        if (!identity) return {};
        const auto sibling = siblingManagerExecutable();
        if (!sibling) return {};
        const auto& executable = *sibling;
        if (!isSingleLinkRegularExecutable(executable)) {
            startupError = "The regular Manager executable beside this connector is unavailable.";
            return {};
        }
        auto homeWide = strictUtf8ToWide(home.value());
        auto persistentRoot = InfrastructureWindows::WindowsAlphaManagerProfile::persistentDataRoot();
        if (!homeWide || !persistentRoot) return {};
        const bool persistent = sameWindowsPath(homeWide.value(), persistentRoot.value());
        auto profile = InfrastructureWindows::WindowsAlphaManagerProfile::create(home);
        if (!profile) return {};
        InfrastructureWindows::WindowsManagerInstanceLeaseOptions options;
        if (!persistent) options.purposeSuffix = profile.value().purposeSuffix();
        const std::wstring registrySubkey = persistent
            ? std::wstring{InfrastructureWindows::DpapiSecureStorage::DefaultRegistrySubkey}
            : std::wstring{profile.value().secureStorageRegistrySubkey()};
        auto names = InfrastructureWindows::WindowsManagerInstanceLease::namesFor(identity.value(), options);
        if (!names) return {};
        const auto readyDeadline = (std::min)(parent.deadline,
            clock->monotonicNow() + std::chrono::seconds{10});
        const auto active = [&] { return !parent.isCancellationRequested() &&
            clock->monotonicNow() < readyDeadline; };
        const auto connect = [&]() -> std::unique_ptr<InfrastructureWindows::WindowsManagerNamedPipeClient> {
            const Domain::OperationContext context{parent.operationId,
                (std::min)(readyDeadline, clock->monotonicNow() + std::chrono::milliseconds{250}),
                parent.cancellation, parent.correlationId};
            InfrastructureWindows::DpapiSecureStorage secure{registrySubkey};
            InfrastructureWindows::WindowsManagerAuthenticationTokenGenerator generator;
            InfrastructureWindows::WindowsManagerAuthenticationTokenStore tokens{secure, generator};
            auto nonce = tokens.load(context);
            if (!nonce || !nonce.value()) return {};
            auto client = InfrastructureWindows::WindowsManagerNamedPipeClient::create(
                clock, std::wstring{names.value().pipeName()}, *nonce.value());
            if (!client) return {};
            auto status = client.value()->status(context);
            if (!status) return {};
            const auto verified = validateDurableManagerStatus(status.value(), home, executable);
            if (!verified) {
                startupError = verified.error().message;
                return {};
            }
            startupError.clear();
            return std::move(client).value();
        };
        if (auto existing = connect()) return existing;
        if (!active()) return {};
        auto startupOptions = options;
        startupOptions.purposeSuffix += persistent ? "startup" : "-startup";
        auto startupLease = InfrastructureWindows::WindowsManagerInstanceLease::acquire(
            identity.value(), startupOptions);
        if (startupLease) {
            if (auto existing = connect()) return existing;
            // A busy, incompatible, or still-starting owner must keep its lease.
            const HANDLE existingLease = ::OpenMutexW(SYNCHRONIZE, FALSE,
                std::wstring{names.value().mutexName()}.c_str());
            if (existingLease) {
                ::CloseHandle(existingLease);
                startupError = "An existing Manager owns this profile but has not passed the matching package, version, and home handshake.";
            } else if (::GetLastError() == ERROR_FILE_NOT_FOUND && active()) {
                auto arguments = quoteWindowsArgument(executable.native());
                arguments += persistent ? L" --home " : L" --alpha-root ";
                arguments += quoteWindowsArgument(homeWide.value());
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process{};
                if (!::CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                        &startup, &process)) {
                    startupError = "Windows could not start the matching Manager (error " +
                        std::to_string(::GetLastError()) + ").";
                    return {};
                }
                ::CloseHandle(process.hThread);
                startupProcess.handle = process.hProcess;
            } else {
                startupError = "The Manager profile ownership could not be checked safely.";
                return {};
            }
        } else if (startupLease.error().code != Domain::ErrorCodes::OwnershipConflict) {
            startupError = startupLease.error().message;
            return {};
        }
        while (active()) {
            if (auto existing = connect()) return existing;
            if (startupProcess.handle) {
                const auto state = ::WaitForSingleObject(startupProcess.handle, 0U);
                if (state == WAIT_OBJECT_0) {
                    DWORD exitCode{};
                    if (!::GetExitCodeProcess(startupProcess.handle, &exitCode)) {
                        startupError = "The Manager startup process exit could not be read (error " +
                            std::to_string(::GetLastError()) + ").";
                        return {};
                    }
                    // A separately launched Manager may have won the profile
                    // lease. Only that observed owner justifies waiting after
                    // this connector's child has already exited.
                    const HANDLE owner = ::OpenMutexW(SYNCHRONIZE, FALSE,
                        std::wstring{names.value().mutexName()}.c_str());
                    if (owner) {
                        ::CloseHandle(owner);
                    } else {
                        const auto nativeError = ::GetLastError();
                        startupError = nativeError == ERROR_FILE_NOT_FOUND
                            ? "The matching durable Manager exited during startup with code " +
                                std::to_string(exitCode) + "."
                            : "The Manager profile owner could not be checked after startup exit (error " +
                                std::to_string(nativeError) + ").";
                        return {};
                    }
                } else if (state == WAIT_FAILED) {
                    startupError = "The Manager startup process could not be observed (error " +
                        std::to_string(::GetLastError()) + ").";
                    return {};
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        if (parent.isCancellationRequested()) startupError = "Manager startup was cancelled; the independent process was left unchanged.";
        return {};
    } catch (...) {
        startupError = "The matching durable Manager startup failed safely.";
        return {};
    }
}

} // namespace

class McpServeCompositionRoot::Impl final {
public:
    explicit Impl(McpServeOptions options)
        : clock_{std::make_shared<InfrastructureWindows::SystemClock>()},
          uuidGenerator_{std::make_shared<
              InfrastructureWindows::WindowsUuidGenerator>()},
          hasher_{std::make_shared<
              InfrastructureWindows::BCryptSha256Hasher>()},
          redactor_{std::make_shared<InfrastructureWindows::SecretRedactor>()},
          unicodeCanonicalizer_{std::make_shared<
              InfrastructureWindows::WindowsUnicodeCanonicalizer>()},
          atomicFileStore_{std::make_shared<
              InfrastructureWindows::WindowsAtomicFileStore>()},
          applicationPaths_{std::make_shared<
              InfrastructureWindows::WindowsApplicationPaths>(
              InfrastructureWindows::WindowsApplicationPathsOptions{
                  std::move(options.explicitHome), true})},
          profile_{resourceProfile()},
          budgets_{Domain::budgetsForProfile(profile_)},
          projectMemoryLimits_{Domain::projectMemoryLimitsForProfile(profile_)},
          runtimeDiagnostics_{std::make_shared<
              InfrastructureWindows::WindowsRuntimeDiagnostics>(
              *clock_, budgets_)},
          processSupervisor_{std::make_shared<
              InfrastructureWindows::WindowsProcessSupervisor>(
              budgets_, runtimeDiagnostics_)},
          role_{configuredRole()},
          clientId_{take(Domain::ClientId::parse(
              nextUuid(*uuidGenerator_).value()))},
          deploymentId_{deploymentIdentity()}
    {
        initialize(options.projectId);
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] int run() noexcept
    {
        if (runInvoked_) {
            std::cerr << "conflict: MCP serve may run only once per process.\n";
            return EXIT_FAILURE;
        }
        runInvoked_ = true;

        try {
            auto startContext = makeContext(
                *uuidGenerator_, *clock_, StartupTimeout,
                "mcp-presence-start");
            requireSuccess(presenceLifecycle_->start(
                Domain::ClientPresenceIdentity{
                    clientId_,
                    std::string{Domain::wireName(role_)},
                    deploymentId_,
                    static_cast<std::uint32_t>(::GetCurrentProcessId())},
                currentDirectory(),
                startContext));
            presenceStarted_ = true;

            Domain::OperationContext serveContext{
                Domain::OperationId{nextUuid(*uuidGenerator_)},
                Domain::MonotonicTimePoint::max(),
                {},
                take(Domain::CorrelationId::parse("mcp-stdio-serve"))};
            auto outcome = server_->run(
                *stdioTransport_, role_, deploymentId_, clientId_, serveContext);
            stopPresence();
            if (!outcome) {
                std::cerr << outcome.error().code << ": "
                          << outcome.error().message << '\n';
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        } catch (const std::exception& error) {
            stopPresence();
            std::cerr << "internal_failure: " << error.what() << '\n';
            return EXIT_FAILURE;
        } catch (...) {
            stopPresence();
            std::cerr << "internal_failure: MCP serve failed safely.\n";
            return EXIT_FAILURE;
        }
    }

private:
    [[nodiscard]] Domain::DeploymentId deploymentIdentity()
    {
        const auto configured = environmentValue(L"FORGE_DEPLOYMENT_ID");
        if (configured && !configured->empty()) {
            return take(Domain::DeploymentId::parse(*configured));
        }
        return take(Domain::DeploymentId::parse(
            nextUuid(*uuidGenerator_).value()));
    }

    void initialize(const std::optional<Domain::ProjectId>& explicitProject)
    {
        const auto startupContext = makeContext(
            *uuidGenerator_, *clock_, StartupTimeout, "mcp-serve-startup");
        const auto dataRoot = take(applicationPaths_->dataRoot(startupContext));
        ensureDirectory(dataRoot);
        const auto configurationRoot =
            take(applicationPaths_->configurationRoot(startupContext));
        ensureDirectory(configurationRoot);
        const auto projectsRoot = childPath(dataRoot, "projects");
        const auto memoryRoot = childPath(dataRoot, "memory");
        const auto handoffsRoot = childPath(memoryRoot, "handoffs");
        ensureDirectory(projectsRoot);
        ensureDirectory(memoryRoot);
        ensureDirectory(handoffsRoot);

        const auto dataAuthorityId = Domain::AuthorityId{
            nextUuid(*uuidGenerator_)};
        const auto dataProjectId = Domain::ProjectId{
            nextUuid(*uuidGenerator_)};
        dataAuthority_ = std::make_shared<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    dataAuthorityId, dataProjectId, clientId_, {dataRoot},
                    Domain::FileAccess::Write,
                    {Domain::FileAccess::Read, Domain::FileAccess::Write,
                     Domain::FileAccess::Create, Domain::FileAccess::Delete},
                    {Domain::FileAccess::Execute}, false)});
        const auto dataScope = take(dataAuthority_->authorityFor(
            dataProjectId, startupContext));

        const auto ledgerPath =
            childPath(memoryRoot, "native-session-ledger.json");
        const auto ledgerRead = authorizePath(
            *dataAuthority_, dataScope, ledgerPath, dataRoot,
            Domain::FileAccess::Read, startupContext);
        auto currentLedger = atomicFileStore_->read(
            ledgerRead,
            InfrastructureWindows::WindowsNativeSessionLedger::
                MaximumDocumentBytes,
            startupContext);
        if (!currentLedger &&
            currentLedger.error().code != Domain::ErrorCodes::RecordNotFound) {
            throw std::runtime_error{
                currentLedger.error().code + ": " +
                currentLedger.error().message};
        }
        if (!currentLedger) {
            const std::array legacyCandidates{
                childPath(dataRoot, "native-session-ledger.json"),
                childPath(dataRoot, "native-session-ledger.json.bak")};
            for (const auto& legacyPath : legacyCandidates) {
                auto legacy = atomicFileStore_->read(
                    authorizePath(
                        *dataAuthority_, dataScope, legacyPath, dataRoot,
                        Domain::FileAccess::Read, startupContext),
                    InfrastructureWindows::WindowsNativeSessionLedger::
                        MaximumDocumentBytes,
                    startupContext);
                if (!legacy) {
                    if (legacy.error().code ==
                        Domain::ErrorCodes::RecordNotFound) {
                        continue;
                    }
                    throw std::runtime_error{
                        legacy.error().code + ": " + legacy.error().message};
                }
                take(atomicFileStore_->replace(
                    authorizePath(
                        *dataAuthority_, dataScope, ledgerPath, dataRoot,
                        Domain::FileAccess::Create, startupContext),
                    legacy.value(), false, startupContext));
                break;
            }
        }

        const auto configPath = childPath(configurationRoot, "config.json");
        configurationStore_ = std::make_unique<
            InfrastructureWindows::WindowsConfigurationStore>(
            *atomicFileStore_,
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Read, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Write, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Create, startupContext),
            authorizePath(
                *dataAuthority_, dataScope,
                pathText(configPath.value() + ".bak"), dataRoot,
                Domain::FileAccess::Read, startupContext));
        configuration_ = take(configurationStore_->load(startupContext));

        const auto registryPath = childPath(projectsRoot, "registry.json");
        projectRegistry_ = std::make_unique<
            PersistenceWindows::WindowsProjectRegistryRepository>(
            applicationPaths_, atomicFileStore_,
            PersistenceWindows::WindowsProjectRegistryStoragePaths{
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Read, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Write, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Create, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope,
                    pathText(registryPath.value() + ".bak"), dataRoot,
                    Domain::FileAccess::Read, startupContext)},
            uuidGenerator_, hasher_, clock_, projectMemoryLimits_);

        const auto startupProject = explicitProject
            ? take(projectRegistry_->descriptor(*explicitProject, startupContext))
            : take(projectRegistry_->initialize(
                Domain::InitializeProjectRequest{currentDirectory(), std::nullopt,
                    std::nullopt, std::nullopt, std::nullopt}, startupContext)).project;
        defaultProjectId_ = startupProject.id;
        if (startupProject.aliases.empty()) {
            throw std::runtime_error{
                "integrity_failure: The startup project has no canonical alias."};
        }
        workspaceAuthority_ = std::make_unique<
            InfrastructureWindows::WindowsProjectWorkspaceAuthority>(
            *projectRegistry_, *uuidGenerator_, clientId_,
            configuration_.shell.enabled, configuration_.allowedRoots, configuration_.fileSystemAccess);

        const auto gitExecutable = discoverGitExecutable();
        const auto powerShellExecutable = discoverPowerShellExecutable();

        auto centralDatabase = take(PersistenceWindows::WindowsCentralDatabase::open(
            applicationPaths_, runtimeDiagnostics_, clock_, startupContext));
        centralDatabase_ = std::shared_ptr<
            PersistenceWindows::WindowsCentralDatabase>{
            std::move(centralDatabase)};
        agentSessionRepository_ = take(
            PersistenceWindows::WindowsAgentSessionRepository::attach(
                centralDatabase_, clock_));
        legacyMemoryRepository_ = take(
            PersistenceWindows::WindowsLegacyMemoryRepository::attach(
                centralDatabase_, clock_, unicodeCanonicalizer_));
        legacyContinuityRepository_ = take(
            PersistenceWindows::WindowsLegacyContinuityRepository::attach(
                centralDatabase_, clock_, hasher_));
        auditRepository_ = take(
            PersistenceWindows::WindowsAuditRepository::attach(
                centralDatabase_));
        forgeStatusRepository_ = take(
            PersistenceWindows::WindowsForgeStatusRepository::attach(
                centralDatabase_));
        presenceRepository_ = take(
            PersistenceWindows::WindowsClientPresenceRepository::attach(
                centralDatabase_));

        fileSystem_ = std::make_shared<NativeToolsWindows::WindowsFileSystem>(
            atomicFileStore_);
        pathGlob_ = std::make_unique<
            NativeToolsWindows::WindowsPathGlobService>();
        textSearch_ = std::make_unique<
            NativeToolsWindows::WindowsTextSearchService>();
        pdf_ = std::make_unique<NativeToolsWindows::WindowsPdfService>(
            *atomicFileStore_);
        artifactDocuments_ = std::make_unique<NativeToolsWindows::WindowsArtifactDocumentService>(
            *workspaceAuthority_, *atomicFileStore_);
        desktopArtifacts_ = std::make_unique<NativeToolsWindows::WindowsDesktopArtifactService>(
            *workspaceAuthority_, *atomicFileStore_);
        webAccess_ = std::make_unique<NativeToolsWindows::WindowsWebAccessService>();
        git_ = std::make_unique<NativeToolsWindows::WindowsGitService>(
            gitExecutable, processSupervisor_);
        shell_ = std::make_unique<NativeToolsWindows::WindowsShellService>(
            powerShellExecutable, processSupervisor_, childPath(dataRoot, "jobs"));

        projectArtifactStore_ = std::make_shared<
            PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            applicationPaths_, uuidGenerator_);
        projectRepositoryOpener_ = std::make_shared<
            PersistenceWindows::WindowsProjectMemoryRepositoryOpener>(
            applicationPaths_, projectArtifactStore_, runtimeDiagnostics_,
            redactor_, hasher_, uuidGenerator_, clock_,
            PersistenceWindows::WindowsProjectMemoryRepositoryOptions{
                {}, projectMemoryLimits_});
        projectRepositoryCache_ = std::make_unique<
            Application::ProjectMemoryRepositoryCache>(
            projectRepositoryOpener_, budgets_.openProjectRepositoriesMaximum);
        projectMemory_ = std::make_unique<Application::ProjectMemoryService>(
            *projectRegistry_, *projectRepositoryCache_, *redactor_,
            projectMemoryLimits_);

        shell_->setJobCompletionSink([this](const Domain::ProjectId& project,
            const Domain::ShellJobSnapshot& job) {
            Domain::ProjectMemoryWrite write;
            write.kind = "process_job_result";
            write.title = "Process job " + job.jobId;
            write.summary = "Durable process outcome; receipt=" + job.receiptPath;
            const nlohmann::json body{{"job_id", job.jobId}, {"receipt_path", job.receiptPath},
                {"stdout_path", job.stdoutPath}, {"stderr_path", job.stderrPath},
                {"log_sha256", job.logHash}, {"log_truncated", job.logTruncated},
                {"exit_code", job.result ? nlohmann::json(job.result->exitCode) : nlohmann::json(nullptr)},
                {"timed_out", job.result && job.result->timedOut},
                {"cancelled", job.result && job.result->cancelled}};
            write.body = body.dump();
            write.tags = {"process", "durable-job", "evidence"};
            write.sourceKind = "forge_process_job";
            write.sourceReference = job.receiptPath;
            const auto operation = makeContext(*uuidGenerator_, *clock_, std::chrono::seconds{30}, "process-job-memory");
            auto saved = projectMemory_->remember({project, std::move(write)}, operation);
            if (!saved) throw std::runtime_error{saved.error().message};
        });
        evidence_ = std::make_unique<NativeToolsWindows::WindowsEvidenceService>(
            *workspaceAuthority_, *atomicFileStore_, *hasher_, *clock_, *uuidGenerator_,
            [this, dataScope, dataRoot, memoryRoot](const Domain::ProjectId& project, const Domain::OperationContext& operation) {
                const auto path = childPath(memoryRoot, "evidence-" + project.value() + ".json");
                return Domain::Result<Contracts::EvidenceStoragePaths>::success({
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Read, operation),
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Write, operation),
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Create, operation)});
            }, dataRoot);
        managerBroker_ = ensureDurableManager(dataRoot, startupContext, clock_, managerStartupError_);
        agentCatalog_ = take(Application::AgentCatalog::create(
            clock_, std::span<const Application::AgentDefinitionDocument>{},
            startupContext));
        reportInspector_ =
            InfrastructureWindows::createWindowsAgentCompletionReportInspector(
                *clock_);
        if (!reportInspector_) {
            throw std::runtime_error{
                "internal_failure: The agent report inspector was not created."};
        }
        agentSessions_ = std::make_unique<Application::AgentSessionService>(
            *agentCatalog_, *agentSessionRepository_, *reportInspector_,
            *workspaceAuthority_, *clock_, *uuidGenerator_,
            configuration_.sessions.idleTimeToLive);
        legacyMemory_ = std::make_unique<Application::LegacyMemoryService>(
            *legacyMemoryRepository_, unicodeCanonicalizer_);

        const auto projectionAuthorityId = Domain::AuthorityId{
            nextUuid(*uuidGenerator_)};
        const auto projectionProjectId = Domain::ProjectId{
            nextUuid(*uuidGenerator_)};
        projectionAuthority_ = std::make_shared<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    projectionAuthorityId, projectionProjectId, clientId_,
                    {memoryRoot}, Domain::FileAccess::Write,
                    {Domain::FileAccess::Read, Domain::FileAccess::Write,
                     Domain::FileAccess::Create, Domain::FileAccess::Delete},
                    {Domain::FileAccess::Execute}, false)});
        const auto projectionScope = take(projectionAuthority_->authorityFor(
            projectionProjectId, startupContext));
        legacyProjectionStore_ = take(
            InfrastructureWindows::WindowsLegacyContinuityProjectionStore::create(
                memoryRoot, handoffsRoot, projectionScope,
                projectionAuthority_, atomicFileStore_, fileSystem_, clock_));
        legacyContinuity_ = std::make_unique<
            Application::LegacyContextContinuityService>(
            *legacyContinuityRepository_, *legacyProjectionStore_,
            *agentSessionRepository_, *clock_, *uuidGenerator_);

        continuityCodec_ = std::make_unique<
            InfrastructureWindows::WindowsContinuityDocumentCodec>(
            hasher_, clock_);
        nativeSessionLedger_ = std::make_unique<
            InfrastructureWindows::WindowsNativeSessionLedger>(
            *atomicFileStore_, *hasher_,
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Read, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Write, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Create, startupContext),
            authorizePath(
                *dataAuthority_, dataScope,
                pathText(ledgerPath.value() + ".bak"), dataRoot,
                Domain::FileAccess::Read, startupContext));
        InfrastructureWindows::LMStudioResponsesTransportConfiguration
            providerConfiguration;
        providerConfiguration.loopbackHost = configuration_.localModel.host;
        providerConfiguration.port = configuration_.localModel.port;
        providerConfiguration.secure = configuration_.localModel.secure;
        providerConfiguration.model = configuration_.localModel.model;
        nativeSessionTransport_ = std::make_unique<
            InfrastructureWindows::LMStudioResponsesTransport>(
            std::move(providerConfiguration));
        nativeSessionAdapter_ = std::make_unique<
            NativeSessionHost::ForgeNativeSessionHostAdapter>(
            take(Domain::AdapterId::parse(
                NativeSessionHost::ForgeNativeSessionHostAdapter::
                    AdapterIdentifier)),
            *nativeSessionLedger_, *nativeSessionTransport_,
            *continuityCodec_, *uuidGenerator_, *clock_);
        continuity_ = std::make_unique<Application::ContinuityCoordinator>(
            *projectRegistry_, *projectRepositoryCache_,
            *nativeSessionAdapter_, *clock_);

        toolCatalog_ = take(Mcp::McpToolCatalog::create());
        clientWorkspaceContext_ = std::make_unique<
            Mcp::McpClientWorkspaceContext>(
            *projectRegistry_, *workspaceAuthority_, *clock_);
        invocationGuard_ = take(Mcp::McpInvocationGuard::create(
            *legacyContinuity_, *hasher_, *clock_));
        policySource_ = std::make_unique<InfrastructureWindows::WindowsPolicySourceReader>();
        projectPolicy_ = std::make_unique<Application::ProjectPolicyService>(
            *policySource_, *atomicFileStore_, *hasher_, *projectRegistry_,
            [this, dataScope, dataRoot, memoryRoot](const Domain::ProjectId& project, const Domain::OperationContext& operation) {
                const auto path = childPath(memoryRoot, "project-policy-" + project.value() + ".json");
                return Domain::Result<Application::PolicyStoragePaths>::success({
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Read, operation),
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Write, operation),
                    authorizePath(*dataAuthority_, dataScope, path, dataRoot, Domain::FileAccess::Create, operation)});
            }, dataRoot.value());
        githubRead_ = std::make_unique<InfrastructureWindows::WindowsGitHubReadService>(
            InfrastructureWindows::WindowsGitHubReadService::configuredEnvironmentToken());
        auto toolDependencies = Mcp::McpToolPackDependencies{
                *toolCatalog_,
                *applicationPaths_,
                *agentCatalog_,
                *agentSessions_,
                *reportInspector_,
                *legacyContinuity_,
                *clientWorkspaceContext_,
                *workspaceAuthority_,
                *fileSystem_,
                *fileSystem_,
                *pathGlob_,
                *git_,
                *legacyMemory_,
                *pdf_,
                *textSearch_,
                *shell_,
                *projectRegistry_,
                *projectMemory_,
                *continuity_,
                *continuityCodec_,
                *invocationGuard_,
                *forgeStatusRepository_,
                *clock_,
                *uuidGenerator_,
                *hasher_,
                projectMemoryLimits_,
                configuration_.shell.defaultTimeout,
                powerShellExecutable,
                std::string{ProductVersion},
                std::string{RuntimeName},
                static_cast<std::uint32_t>(::GetCurrentProcessId()), projectPolicy_.get(),
                explicitProject ? "explicit_project_id" : "process_working_directory"};
        toolDependencies.evidence = evidence_.get();
        toolDependencies.webAccess = webAccess_.get();
        toolDependencies.artifactDocuments = artifactDocuments_.get();
        toolDependencies.desktopArtifacts = desktopArtifacts_.get();
        toolDependencies.managerStartupError = managerStartupError_;
        toolDependencies.visibleChatRemoteStatus = [this, dataRoot](
            const Domain::ProjectId& project, const Domain::OperationContext& operation) {
            if (!managerBroker_) {
                return Domain::Result<std::string>::failure(Domain::makeError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The Manager-owned native observer is unavailable."));
            }
            const Domain::OperationContext brokerOperation{
                operation.operationId,
                (std::min)(operation.deadline, clock_->monotonicNow() +
                    ForgeConductor::Manager::ManagerTransportLimits::DefaultMaximumRequestLifetime),
                operation.cancellation,
                operation.correlationId};
            auto managerStatus = managerBroker_->status(brokerOperation);
            if (!managerStatus) {
                return Domain::Result<std::string>::failure(managerStatus.error());
            }
            auto executable = siblingManagerExecutable();
            if (!executable) {
                return Domain::Result<std::string>::failure(Domain::makeError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The Manager sibling could not be resolved."));
            }
            auto verified = validateDurableManagerStatus(
                managerStatus.value(), dataRoot, *executable);
            if (!verified) {
                return Domain::Result<std::string>::failure(verified.error());
            }
            auto observed = managerBroker_->visibleChatStatus(project, brokerOperation);
            if (!observed) {
                return Domain::Result<std::string>::failure(observed.error());
            }
            return Domain::Result<std::string>::success(observed.value().canonicalStatus);
        };
        if (role_ == Domain::McpRole::Primary) {
            toolDependencies.visibleChatObservation = [this, dataRoot](
                const Domain::ProjectId& project, const Domain::PathText& root,
                std::string_view name, bool succeeded, std::string_view payload,
                const Domain::OperationContext& operation) {
                if (!managerBroker_) {
                    return;
                }
                const Domain::OperationContext brokerOperation{
                    operation.operationId,
                    (std::min)(operation.deadline, clock_->monotonicNow() +
                        ForgeConductor::Manager::ManagerTransportLimits::DefaultMaximumRequestLifetime),
                    operation.cancellation,
                    operation.correlationId};
                auto managerStatus = managerBroker_->status(brokerOperation);
                auto executable = siblingManagerExecutable();
                if (!managerStatus || !executable ||
                    !validateDurableManagerStatus(managerStatus.value(), dataRoot, *executable)) {
                    return;
                }
                const auto observed = managerBroker_->visibleChatObserve(
                    {project, root, std::string{name}, succeeded, std::string{payload}},
                    brokerOperation);
                // The Manager verifies renderer evidence without changing a completed tool result.
                if (!observed) {
                    std::cerr << "visible_chat_bridge: " << observed.error().code << ": "
                              << observed.error().message << '\n';
                }
            };
        }
        if (managerBroker_) toolDependencies.durableToolBroker = [this, dataRoot](
            const std::string_view name, const std::string_view arguments,
            const Domain::ProjectId& project, const Domain::OperationContext& operation) {
            const Domain::OperationContext brokerOperation{operation.operationId,
                (std::min)(operation.deadline, clock_->monotonicNow() +
                    ForgeConductor::Manager::ManagerTransportLimits::DefaultMaximumRequestLifetime),
                operation.cancellation, operation.correlationId};
            auto status = managerBroker_->status(brokerOperation);
            if (!status) return Domain::Result<std::string>::failure(status.error());
            const auto executable = siblingManagerExecutable();
            if (!executable) return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The durable Manager sibling path could not be resolved."));
            const auto verified = validateDurableManagerStatus(status.value(), dataRoot, *executable);
            if (!verified) return Domain::Result<std::string>::failure(verified.error());
            auto result = managerBroker_->invokeTool(
                {project, std::string{name}, std::string{arguments}}, brokerOperation);
            if (!result) return Domain::Result<std::string>::failure(result.error());
            if (!result.value().ok && result.value().error)
                return Domain::Result<std::string>::failure(*result.value().error);
            return Domain::Result<std::string>::success(result.value().canonicalPayload);
        };
        toolDependencies.providerInspection = [this](const Domain::OperationContext& operation) {
            auto current = configurationStore_->reload(operation);
            if (!current) return Domain::Result<std::string>::failure(current.error());
            InfrastructureWindows::LMStudioResponsesTransportConfiguration configuration;
            configuration.loopbackHost = current.value().localModel.host;
            configuration.port = current.value().localModel.port;
            configuration.secure = current.value().localModel.secure;
            configuration.model = current.value().localModel.model;
            configuration.connectTimeout = std::chrono::seconds{5};
            configuration.sendTimeout = std::chrono::seconds{5};
            configuration.receiveTimeout = std::chrono::seconds{5};
            InfrastructureWindows::LMStudioResponsesTransport inspection{std::move(configuration)};
            auto inspected = inspection.inspect(current.value().localModel, operation);
            if (!inspected) return inspected;
            auto payload = nlohmann::json::parse(inspected.value());
            auto desktopVersion = InfrastructureWindows::WindowsSystemInspection::lmStudioDesktopVersion(operation);
            if (!desktopVersion) return Domain::Result<std::string>::failure(desktopVersion.error());
            payload["lm_studio_desktop_version"] = nlohmann::json::parse(desktopVersion.value());
            return Domain::Result<std::string>::success(payload.dump());
        };
        toolDependencies.systemInspection = [](const Domain::OperationContext& operation) {
            return InfrastructureWindows::WindowsSystemInspection::inspect(operation);
        };
        toolDependencies.githubRead = githubRead_.get();
        toolPack_ = take(Mcp::McpToolPackAdapter::create(std::move(toolDependencies)));
        toolAuthorizer_ = std::make_unique<Mcp::McpToolAuthorizer>(*clock_, projectPolicy_.get());
        const std::array<Contracts::IToolHandler*, 1U> handlers{toolPack_.get()};
        toolRouter_ = take(Mcp::McpToolRouter::create(
            *toolCatalog_, handlers, *toolAuthorizer_, *invocationGuard_,
            *auditRepository_, *hasher_, *clock_));
        executionContextResolver_ = std::make_unique<
            Mcp::McpExecutionContextResolver>(
            *workspaceAuthority_, defaultProjectId_, *clock_,
            clientWorkspaceContext_.get());
        auto bootstrapInstructions = take(toolPack_->bootstrapInstructions(
            defaultProjectId_, startupProject.aliases.front(),
            startupContext));
        if (bootstrapInstructions.empty() ||
            bootstrapInstructions.size() >
                Mcp::McpServer::MaximumInstructionsBytes) {
            throw std::runtime_error{
                "integrity_failure: The MCP workspace instructions exceeded their bound."};
        }
        server_ = std::make_unique<Mcp::McpServer>(
            *toolCatalog_, *toolRouter_, *executionContextResolver_,
            *uuidGenerator_, *clock_, std::move(bootstrapInstructions));
        stdioTransport_ = take(Mcp::WindowsStdioMcpTransport::create());
        presenceLifecycle_ = std::make_unique<
            Application::ClientPresenceLifecycle>(
            presenceRepository_, clock_, uuidGenerator_, runtimeDiagnostics_);
    }

    void stopPresence() noexcept
    {
        if (!presenceStarted_ || !presenceLifecycle_) {
            return;
        }
        try {
            const auto context = makeContext(
                *uuidGenerator_, *clock_, ShutdownTimeout,
                "mcp-presence-stop");
            const auto stopped = presenceLifecycle_->stop(context);
            if (!stopped) {
                std::cerr << stopped.error().code << ": "
                          << stopped.error().message << '\n';
            }
        } catch (...) {
        }
        presenceStarted_ = false;
    }

    void shutdown() noexcept
    {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        stopPresence();
        presenceLifecycle_.reset();

        if (server_) {
            server_->shutdown();
        }
        if (stdioTransport_) {
            stdioTransport_->shutdown();
        }
        server_.reset();
        stdioTransport_.reset();
        if (toolRouter_) {
            toolRouter_->shutdown();
        }
        toolRouter_.reset();
        toolPack_.reset();
        githubRead_.reset();
        executionContextResolver_.reset();
        toolAuthorizer_.reset();
        if (invocationGuard_) {
            invocationGuard_->shutdown();
        }
        invocationGuard_.reset();
        if (clientWorkspaceContext_) {
            clientWorkspaceContext_->shutdown();
        }
        clientWorkspaceContext_.reset();
        toolCatalog_.reset();

        if (continuity_) {
            continuity_->shutdown();
        }
        continuity_.reset();
        if (nativeSessionAdapter_) {
            nativeSessionAdapter_->shutdown();
        }
        nativeSessionAdapter_.reset();
        if (nativeSessionTransport_) {
            nativeSessionTransport_->shutdown();
        }
        nativeSessionTransport_.reset();
        if (nativeSessionLedger_) {
            nativeSessionLedger_->shutdown();
        }
        nativeSessionLedger_.reset();
        continuityCodec_.reset();

        if (legacyContinuity_) {
            legacyContinuity_->shutdown();
        }
        legacyContinuity_.reset();
        if (legacyProjectionStore_) {
            legacyProjectionStore_->close();
        }
        legacyProjectionStore_.reset();
        projectionAuthority_.reset();

        if (agentSessions_) {
            agentSessions_->shutdown();
        }
        agentSessions_.reset();
        reportInspector_.reset();
        agentCatalog_.reset();
        if (legacyMemory_) {
            legacyMemory_->shutdown();
        }
        legacyMemory_.reset();

        if (shell_) shell_->shutdown();
        if (projectMemory_) {
            projectMemory_->shutdown();
        }
        projectMemory_.reset();
        if (projectRepositoryCache_) {
            projectRepositoryCache_->shutdown();
        }
        projectRepositoryCache_.reset();
        if (projectRepositoryOpener_) {
            projectRepositoryOpener_->shutdown();
        }
        projectRepositoryOpener_.reset();
        projectArtifactStore_.reset();

        shell_.reset();
        git_.reset();
        pdf_.reset();
        artifactDocuments_.reset();
        desktopArtifacts_.reset();
        webAccess_.reset();
        textSearch_.reset();
        pathGlob_.reset();
        fileSystem_.reset();
        if (processSupervisor_) {
            processSupervisor_->shutdown();
        }

        if (presenceRepository_) {
            presenceRepository_->close();
        }
        if (forgeStatusRepository_) {
            forgeStatusRepository_->close();
        }
        if (auditRepository_) {
            auditRepository_->close();
        }
        if (legacyContinuityRepository_) {
            legacyContinuityRepository_->close();
        }
        if (legacyMemoryRepository_) {
            legacyMemoryRepository_->close();
        }
        if (agentSessionRepository_) {
            agentSessionRepository_->close();
        }
        presenceRepository_.reset();
        forgeStatusRepository_.reset();
        auditRepository_.reset();
        legacyContinuityRepository_.reset();
        legacyMemoryRepository_.reset();
        agentSessionRepository_.reset();

        workspaceAuthority_.reset();
        projectRegistry_.reset();
        if (configurationStore_) {
            configurationStore_->shutdown();
        }
        configurationStore_.reset();
        dataAuthority_.reset();

        if (centralDatabase_) {
            try {
                const auto context = makeContext(
                    *uuidGenerator_, *clock_, ShutdownTimeout,
                    "mcp-central-database-close");
                static_cast<void>(centralDatabase_->close(context));
            } catch (...) {
            }
        }
        centralDatabase_.reset();
        processSupervisor_.reset();
        if (runtimeDiagnostics_) {
            runtimeDiagnostics_->shutdown();
        }
        runtimeDiagnostics_.reset();
        applicationPaths_.reset();
        atomicFileStore_.reset();
        unicodeCanonicalizer_.reset();
        redactor_.reset();
        hasher_.reset();
        uuidGenerator_.reset();
        clock_.reset();
    }

    std::shared_ptr<InfrastructureWindows::SystemClock> clock_;
    std::shared_ptr<InfrastructureWindows::WindowsUuidGenerator> uuidGenerator_;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher_;
    std::shared_ptr<InfrastructureWindows::SecretRedactor> redactor_;
    std::shared_ptr<InfrastructureWindows::WindowsUnicodeCanonicalizer>
        unicodeCanonicalizer_;
    std::shared_ptr<InfrastructureWindows::WindowsAtomicFileStore>
        atomicFileStore_;
    std::shared_ptr<InfrastructureWindows::WindowsApplicationPaths>
        applicationPaths_;
    Domain::ResourceProfile profile_;
    Domain::ResourceBudgets budgets_;
    Domain::ProjectMemoryLimits projectMemoryLimits_;
    std::shared_ptr<InfrastructureWindows::WindowsRuntimeDiagnostics>
        runtimeDiagnostics_;
    std::shared_ptr<InfrastructureWindows::WindowsProcessSupervisor>
        processSupervisor_;
    Domain::McpRole role_;
    Domain::ClientId clientId_;
    Domain::DeploymentId deploymentId_;
    Domain::AppConfig configuration_;
    Domain::ProjectId defaultProjectId_{Domain::Uuid::parse(
        "00000000-0000-4000-8000-000000000000").value()};

    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        dataAuthority_;
    std::unique_ptr<InfrastructureWindows::WindowsProjectWorkspaceAuthority>
        workspaceAuthority_;
    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        projectionAuthority_;
    std::unique_ptr<InfrastructureWindows::WindowsConfigurationStore>
        configurationStore_;
    std::unique_ptr<PersistenceWindows::WindowsProjectRegistryRepository>
        projectRegistry_;
    std::shared_ptr<PersistenceWindows::WindowsCentralDatabase> centralDatabase_;
    std::shared_ptr<PersistenceWindows::WindowsAgentSessionRepository>
        agentSessionRepository_;
    std::shared_ptr<PersistenceWindows::WindowsLegacyMemoryRepository>
        legacyMemoryRepository_;
    std::shared_ptr<PersistenceWindows::WindowsLegacyContinuityRepository>
        legacyContinuityRepository_;
    std::shared_ptr<PersistenceWindows::WindowsAuditRepository> auditRepository_;
    std::shared_ptr<PersistenceWindows::WindowsForgeStatusRepository>
        forgeStatusRepository_;
    std::shared_ptr<PersistenceWindows::WindowsClientPresenceRepository>
        presenceRepository_;

    std::shared_ptr<NativeToolsWindows::WindowsFileSystem> fileSystem_;
    std::unique_ptr<NativeToolsWindows::WindowsPathGlobService> pathGlob_;
    std::unique_ptr<NativeToolsWindows::WindowsTextSearchService> textSearch_;
    std::unique_ptr<NativeToolsWindows::WindowsPdfService> pdf_;
    std::unique_ptr<NativeToolsWindows::WindowsArtifactDocumentService> artifactDocuments_;
    std::unique_ptr<NativeToolsWindows::WindowsDesktopArtifactService> desktopArtifacts_;
    std::unique_ptr<NativeToolsWindows::WindowsWebAccessService> webAccess_;
    std::unique_ptr<NativeToolsWindows::WindowsGitService> git_;
    std::unique_ptr<NativeToolsWindows::WindowsShellService> shell_;
    std::unique_ptr<NativeToolsWindows::WindowsEvidenceService> evidence_;
    std::unique_ptr<InfrastructureWindows::WindowsGitHubReadService> githubRead_;
    std::unique_ptr<InfrastructureWindows::WindowsManagerNamedPipeClient> managerBroker_;
    std::string managerStartupError_;

    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryArtifactStore>
        projectArtifactStore_;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepositoryOpener>
        projectRepositoryOpener_;
    std::unique_ptr<Application::ProjectMemoryRepositoryCache>
        projectRepositoryCache_;
    std::unique_ptr<Application::ProjectMemoryService> projectMemory_;
    std::unique_ptr<Application::AgentCatalog> agentCatalog_;
    std::unique_ptr<Contracts::IAgentCompletionReportInspector> reportInspector_;
    std::unique_ptr<Application::AgentSessionService> agentSessions_;
    std::unique_ptr<Application::LegacyMemoryService> legacyMemory_;
    std::shared_ptr<
        InfrastructureWindows::WindowsLegacyContinuityProjectionStore>
        legacyProjectionStore_;
    std::unique_ptr<Application::LegacyContextContinuityService>
        legacyContinuity_;
    std::unique_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec>
        continuityCodec_;
    std::unique_ptr<InfrastructureWindows::WindowsNativeSessionLedger>
        nativeSessionLedger_;
    std::unique_ptr<InfrastructureWindows::LMStudioResponsesTransport>
        nativeSessionTransport_;
    std::unique_ptr<NativeSessionHost::ForgeNativeSessionHostAdapter>
        nativeSessionAdapter_;
    std::unique_ptr<Application::ContinuityCoordinator> continuity_;

    std::unique_ptr<Mcp::McpToolCatalog> toolCatalog_;
    std::unique_ptr<Mcp::McpClientWorkspaceContext> clientWorkspaceContext_;
    std::unique_ptr<Mcp::McpInvocationGuard> invocationGuard_;
    std::unique_ptr<Mcp::McpToolPackAdapter> toolPack_;
    std::unique_ptr<Mcp::McpToolAuthorizer> toolAuthorizer_;
    std::unique_ptr<InfrastructureWindows::WindowsPolicySourceReader> policySource_;
    std::unique_ptr<Application::ProjectPolicyService> projectPolicy_;
    std::unique_ptr<Mcp::McpToolRouter> toolRouter_;
    std::unique_ptr<Mcp::McpExecutionContextResolver>
        executionContextResolver_;
    std::unique_ptr<Mcp::McpServer> server_;
    std::unique_ptr<Mcp::WindowsStdioMcpTransport> stdioTransport_;
    std::unique_ptr<Application::ClientPresenceLifecycle> presenceLifecycle_;

    bool presenceStarted_{};
    bool runInvoked_{};
    bool shutdown_{};
};

McpServeCompositionRoot::McpServeCompositionRoot(McpServeOptions options)
    : implementation_{std::make_unique<Impl>(std::move(options))}
{
}

McpServeCompositionRoot::~McpServeCompositionRoot() noexcept = default;

int McpServeCompositionRoot::run() noexcept
{
    return implementation_ ? implementation_->run() : EXIT_FAILURE;
}

} // namespace ForgeConductor::Hosts::Cli
