#include "ManagerProcessEnvironment.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "../../Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "../../Infrastructure/Windows/Detail/UniqueHandle.h"
#include "../../Infrastructure/Windows/Detail/UtfConversion.h"
#include "../../Infrastructure/Windows/Detail/Win32Error.h"
#include "../../Infrastructure/Windows/Detail/WindowsPathResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Composition::Windows {
namespace {

namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace WindowsDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

constexpr std::wstring_view ExpectedManagerImageName =
    L"ForgeConductor.Manager.exe";
constexpr std::string_view CliImageName = "forge-conductor.exe";
constexpr std::wstring_view ExtendedPathPrefix = L"\\\\?\\";
constexpr std::wstring_view ExtendedUncPrefix = L"\\\\?\\UNC\\";

[[nodiscard]] Domain::Error internalFailure(std::string message) noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::move(message));
}

[[nodiscard]] Domain::Error integrityFailure(std::string message) noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return WindowsDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

[[nodiscard]] Domain::Result<Domain::PathText> resolveChildRoot(
    const Domain::PathText& dataRoot,
    const std::wstring_view child) noexcept
{
    try {
        auto rootWide = WindowsDetail::strictUtf8ToUtf16(dataRoot.value());
        if (!rootWide) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(rootWide).error());
        }
        auto resolved = WindowsDetail::WindowsPathResolver::resolveAppOwnedChild(
            rootWide.value(), child,
            WindowsDetail::MissingPathPolicy::AllowDescendants);
        if (!resolved) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(resolved).error());
        }
        return WindowsDetail::WindowsPathResolver::toPathText(resolved.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(internalFailure(
            "The Manager process child data root could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<void> validateCanonicalLocalPath(
    const Domain::PathText& path,
    const std::string_view evidenceName) noexcept
{
    try {
        auto resolved = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
            path.value());
        if (!resolved) {
            return Domain::Result<void>::failure(std::move(resolved).error());
        }
        auto canonical =
            WindowsDetail::WindowsPathResolver::toPathText(resolved.value());
        if (!canonical) {
            return Domain::Result<void>::failure(std::move(canonical).error());
        }
        if (canonical.value() != path) {
            return Domain::Result<void>::failure(integrityFailure(
                std::string{evidenceName} +
                " was not returned as a canonical local Windows path."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalFailure(
            "The Manager process executable path could not be validated."));
    }
}

[[nodiscard]] bool equalWindowsPath(
    const Domain::PathText& left,
    const Domain::PathText& right) noexcept
{
    try {
        auto leftWide = WindowsDetail::strictUtf8ToUtf16(left.value());
        auto rightWide = WindowsDetail::strictUtf8ToUtf16(right.value());
        if (!leftWide || !rightWide ||
            leftWide.value().size() != rightWide.value().size()) {
            return false;
        }
        return ::CompareStringOrdinal(
                   leftWide.value().data(),
                   static_cast<int>(leftWide.value().size()),
                   rightWide.value().data(),
                   static_cast<int>(rightWide.value().size()), TRUE) ==
            CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] Domain::Result<void> validateExecutableIdentity(
    const ManagerProcessExecutableIdentity& identity,
    const std::wstring_view expectedLeaf,
    const std::string_view evidenceName) noexcept
{
    try {
        auto validPath =
            validateCanonicalLocalPath(identity.canonicalPath, evidenceName);
        if (!validPath) {
            return validPath;
        }
        auto wide =
            WindowsDetail::strictUtf8ToUtf16(identity.canonicalPath.value());
        if (!wide) {
            return Domain::Result<void>::failure(std::move(wide).error());
        }
        const std::size_t separator = wide.value().find_last_of(L'\\');
        if (separator == std::wstring::npos ||
            separator + 1U >= wide.value().size()) {
            return Domain::Result<void>::failure(integrityFailure(
                std::string{evidenceName} +
                " did not contain an executable filename."));
        }
        const std::wstring_view leaf{wide.value().data() + separator + 1U,
                                     wide.value().size() - separator - 1U};
        if (leaf.size() != expectedLeaf.size() ||
            ::CompareStringOrdinal(
                leaf.data(), static_cast<int>(leaf.size()),
                expectedLeaf.data(), static_cast<int>(expectedLeaf.size()),
                TRUE) != CSTR_EQUAL) {
            return Domain::Result<void>::failure(integrityFailure(
                std::string{evidenceName} +
                " did not have the required executable filename."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalFailure(
            "The Manager process executable identity could not be validated."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> siblingCliPath(
    const Domain::PathText& managerImage) noexcept
{
    try {
        const std::size_t separator = managerImage.value().find_last_of('\\');
        if (separator == std::string::npos || separator < 2U) {
            return Domain::Result<Domain::PathText>::failure(integrityFailure(
                "The Manager image path has no bounded sibling directory."));
        }
        std::string sibling = managerImage.value().substr(0U, separator + 1U);
        sibling.append(CliImageName);
        auto result = Domain::PathText::create(sibling);
        if (!result) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(result).error());
        }
        return result;
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(internalFailure(
            "The sibling Forge Conductor executable path could not be built."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> extendedPath(
    const Domain::PathText& path) noexcept
{
    try {
        auto wide = WindowsDetail::strictUtf8ToUtf16(path.value());
        if (!wide) {
            return Domain::Result<std::wstring>::failure(
                std::move(wide).error());
        }
        if (wide.value().starts_with(L"\\\\") ||
            wide.value().starts_with(ExtendedPathPrefix)) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Manager process paths must use a local DOS drive path."));
        }
        std::wstring native{ExtendedPathPrefix};
        native.append(wide.value());
        return Domain::Result<std::wstring>::success(std::move(native));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(internalFailure(
            "The Manager process native path could not be constructed."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> canonicalPathForHandle(
    const HANDLE handle) noexcept
{
    try {
        const DWORD required = ::GetFinalPathNameByHandleW(
            handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U ||
            required > WindowsManagerProcessEnvironmentPlatformProbe::
                           MaximumExecutablePathCharacters +
                    ExtendedPathPrefix.size()) {
            return Domain::Result<std::wstring>::failure(
                WindowsDetail::makeWin32Error(
                    "resolve a Manager process executable path",
                    ::GetLastError()));
        }
        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<std::wstring>::failure(
                WindowsDetail::makeWin32Error(
                    "resolve a Manager process executable path",
                    ::GetLastError()));
        }

        std::wstring canonical{
            buffer.data(), static_cast<std::size_t>(written)};
        if (canonical.starts_with(ExtendedUncPrefix)) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Manager process executables cannot reside on a network path."));
        }
        if (canonical.starts_with(ExtendedPathPrefix)) {
            canonical.erase(0U, ExtendedPathPrefix.size());
        }
        return Domain::Result<std::wstring>::success(std::move(canonical));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(internalFailure(
            "The opened Manager process executable path could not be retained."));
    }
}

[[nodiscard]] std::string_view missingFileCode(const DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? Domain::ErrorCodes::RecordNotFound
        : Domain::ErrorCodes::InternalFailure;
}

[[nodiscard]] Domain::Result<ManagerProcessExecutableIdentity>
regularExecutableIdentity(
    const Domain::PathText& executable,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = validateContext(
            context, "inspect a Manager process executable identity");
        if (!active) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(active).error());
        }
        auto validPath = validateCanonicalLocalPath(
            executable, "The requested Manager process executable");
        if (!validPath) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(validPath).error());
        }
        auto native = extendedPath(executable);
        if (!native) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(native).error());
        }

        ::SetLastError(ERROR_SUCCESS);
        WindowsDetail::UniqueHandle handle{::CreateFileW(
            native.value().c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!handle) {
            const DWORD error = ::GetLastError();
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                WindowsDetail::makeWin32Error(
                    "open a Manager process executable", error,
                    missingFileCode(error)));
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (::GetFileInformationByHandleEx(
                handle.get(), FileAttributeTagInfo, &attributes,
                sizeof(attributes)) == FALSE) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                WindowsDetail::makeWin32Error(
                    "inspect a Manager process executable",
                    ::GetLastError()));
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "A Manager process executable cannot be a reparse point."));
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                integrityFailure(
                    "A Manager process executable resolved to a directory."));
        }

        auto canonicalWide = canonicalPathForHandle(handle.get());
        if (!canonicalWide) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(canonicalWide).error());
        }
        auto canonicalUtf8 =
            WindowsDetail::strictUtf16ToUtf8(canonicalWide.value());
        if (!canonicalUtf8) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(canonicalUtf8).error());
        }
        auto canonicalPath = Domain::PathText::create(canonicalUtf8.value());
        if (!canonicalPath) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                std::move(canonicalPath).error());
        }
        if (!equalWindowsPath(executable, canonicalPath.value())) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The opened Manager process executable differs from its requested canonical path."));
        }

        FILE_ID_INFO fileInformation{};
        if (::GetFileInformationByHandleEx(
                handle.get(), FileIdInfo, &fileInformation,
                sizeof(fileInformation)) == FALSE) {
            return Domain::Result<ManagerProcessExecutableIdentity>::failure(
                WindowsDetail::makeWin32Error(
                    "read a Manager process executable file identity",
                    ::GetLastError()));
        }
        std::array<std::byte, 16U> identifier{};
        for (std::size_t index = 0U; index < identifier.size(); ++index) {
            identifier[index] = static_cast<std::byte>(
                fileInformation.FileId.Identifier[index]);
        }
        return Domain::Result<ManagerProcessExecutableIdentity>::success(
            ManagerProcessExecutableIdentity{
                std::move(canonicalPath).value(),
                static_cast<std::uint64_t>(
                    fileInformation.VolumeSerialNumber),
                identifier});
    } catch (...) {
        return Domain::Result<ManagerProcessExecutableIdentity>::failure(
            internalFailure(
                "The Manager process executable identity probe failed safely."));
    }
}

[[nodiscard]] Domain::Error directoryCreationError(
    const DWORD error) noexcept
{
    if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
        return WindowsDetail::makeWin32Error(
            "create an exact Manager process directory", error,
            Domain::ErrorCodes::RecordNotFound);
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
        return WindowsDetail::makeWin32Error(
            "create an exact Manager process directory", error,
            Domain::ErrorCodes::Unauthorized);
    }
    return WindowsDetail::makeWin32Error(
        "create an exact Manager process directory", error);
}

} // namespace

ManagerProcessEnvironmentSnapshot::ManagerProcessEnvironmentSnapshot(
    Domain::PathText dataRoot,
    Domain::PathText configurationRoot,
    Domain::PathText diagnosticsRoot,
    Domain::PathText exportRoot,
    Domain::PathText projectsRoot,
    ManagerProcessExecutableIdentity managerExecutableIdentity,
    ManagerProcessExecutableIdentity cliExecutableIdentity,
    const std::uint64_t physicalMemoryBytes,
    const Domain::ResourceProfile resourceProfile,
    Domain::ResourceBudgets resourceBudgets) noexcept
    : dataRoot_{std::move(dataRoot)},
      configurationRoot_{std::move(configurationRoot)},
      diagnosticsRoot_{std::move(diagnosticsRoot)},
      exportRoot_{std::move(exportRoot)},
      projectsRoot_{std::move(projectsRoot)},
      managerExecutableIdentity_{std::move(managerExecutableIdentity)},
      cliExecutableIdentity_{std::move(cliExecutableIdentity)},
      physicalMemoryBytes_{physicalMemoryBytes},
      resourceProfile_{resourceProfile},
      resourceBudgets_{std::move(resourceBudgets)}
{
}

PreparedManagerProcessEnvironment::PreparedManagerProcessEnvironment(
    ManagerProcessEnvironmentSnapshot snapshot,
    InfrastructureWindows::WindowsManagerInstanceLease lease) noexcept
    : snapshot_{std::move(snapshot)}, lease_{std::move(lease)}
{
}

ManagerProcessEnvironment::ManagerProcessEnvironment(
    ManagerProcessEnvironmentOptions options,
    IManagerProcessEnvironmentPlatformProbe& platformProbe) noexcept
    : options_{std::move(options)}, platformProbe_{platformProbe}
{
}

Domain::Result<ManagerProcessPlatformSnapshot>
WindowsManagerProcessEnvironmentPlatformProbe::inspect(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active =
            validateContext(context, "inspect the Manager process platform");
        if (!active) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                std::move(active).error());
        }

        std::array<wchar_t, MaximumExecutablePathCharacters> imageBuffer{};
        ::SetLastError(ERROR_SUCCESS);
        const DWORD imageCharacters = ::GetModuleFileNameW(
            nullptr, imageBuffer.data(),
            static_cast<DWORD>(imageBuffer.size()));
        if (imageCharacters == 0U || imageCharacters >= imageBuffer.size()) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "GetModuleFileNameW could not return a bounded Manager image path."));
        }
        auto imageUtf8 = WindowsDetail::strictUtf16ToUtf8(
            std::wstring_view{imageBuffer.data(),
                              static_cast<std::size_t>(imageCharacters)});
        if (!imageUtf8) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                std::move(imageUtf8).error());
        }
        auto imagePath = Domain::PathText::create(imageUtf8.value());
        if (!imagePath) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                std::move(imagePath).error());
        }
        auto imageIdentity = regularExecutableIdentity(
            imagePath.value(), context);
        if (!imageIdentity) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                std::move(imageIdentity).error());
        }

        active =
            validateContext(context, "inspect Manager process physical memory");
        if (!active) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                std::move(active).error());
        }
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (::GlobalMemoryStatusEx(&memory) == FALSE) {
            return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
                WindowsDetail::makeWin32Error(
                    "read Manager process physical memory", ::GetLastError()));
        }
        return Domain::Result<ManagerProcessPlatformSnapshot>::success(
            ManagerProcessPlatformSnapshot{
                std::move(imageIdentity).value(),
                static_cast<std::uint64_t>(memory.ullTotalPhys)});
    } catch (...) {
        return Domain::Result<ManagerProcessPlatformSnapshot>::failure(
            internalFailure(
                "The Manager process platform probe failed safely."));
    }
}

Domain::Result<ManagerProcessExecutableIdentity>
WindowsManagerProcessEnvironmentPlatformProbe::executableIdentity(
    const Domain::PathText& executable,
    const Domain::OperationContext& context) noexcept
{
    return regularExecutableIdentity(executable, context);
}

Domain::Result<void>
WindowsManagerProcessEnvironmentPlatformProbe::ensureRegularDirectory(
    const Domain::PathText& directory,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = validateContext(
            context, "prepare an exact Manager process directory");
        if (!active) {
            return active;
        }
        auto validPath = validateCanonicalLocalPath(
            directory, "The exact Manager process directory");
        if (!validPath) {
            return validPath;
        }
        auto native = extendedPath(directory);
        if (!native) {
            return Domain::Result<void>::failure(std::move(native).error());
        }

        if (::CreateDirectoryW(native.value().c_str(), nullptr) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                return Domain::Result<void>::failure(
                    directoryCreationError(error));
            }
        }

        active = validateContext(
            context, "validate an exact Manager process directory");
        if (!active) {
            return active;
        }
        const DWORD attributes =
            ::GetFileAttributesW(native.value().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return Domain::Result<void>::failure(
                WindowsDetail::makeWin32Error(
                    "validate an exact Manager process directory",
                    ::GetLastError()));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "An exact Manager process directory cannot be a reparse point."));
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "An exact Manager process directory path is occupied by a file."));
        }

        auto revalidated =
            WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
                directory.value());
        if (!revalidated) {
            return Domain::Result<void>::failure(
                std::move(revalidated).error());
        }
        auto canonical = WindowsDetail::WindowsPathResolver::toPathText(
            revalidated.value());
        if (!canonical) {
            return Domain::Result<void>::failure(
                std::move(canonical).error());
        }
        if (canonical.value() != directory) {
            return Domain::Result<void>::failure(integrityFailure(
                "An exact Manager process directory changed during creation."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalFailure(
            "The exact Manager process directory preparation failed safely."));
    }
}

Domain::Result<ManagerProcessEnvironmentSnapshot>
ManagerProcessEnvironment::inspect(
    const Domain::OperationContext& context) const noexcept
{
    try {
        auto active =
            validateContext(context, "inspect the Manager process environment");
        if (!active) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(active).error());
        }

        InfrastructureWindows::WindowsApplicationPaths paths{
            InfrastructureWindows::WindowsApplicationPathsOptions{
                options_.explicitDataRoot,
                options_.allowEnvironmentOverride}};
        auto dataRoot = paths.dataRoot(context);
        if (!dataRoot) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(dataRoot).error());
        }
        auto configurationRoot = paths.configurationRoot(context);
        if (!configurationRoot) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(configurationRoot).error());
        }
        auto diagnosticsRoot = paths.diagnosticsRoot(context);
        if (!diagnosticsRoot) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(diagnosticsRoot).error());
        }
        auto exportRoot = resolveChildRoot(dataRoot.value(), L"exports");
        if (!exportRoot) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(exportRoot).error());
        }
        auto projectsRoot = resolveChildRoot(dataRoot.value(), L"projects");
        if (!projectsRoot) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(projectsRoot).error());
        }

        auto platform = platformProbe_.inspect(context);
        if (!platform) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(platform).error());
        }
        if (platform.value().physicalMemoryBytes == 0U) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                integrityFailure(
                    "The Manager process platform probe reported zero physical memory."));
        }
        auto validManager = validateExecutableIdentity(
            platform.value().currentManagerImage,
            ExpectedManagerImageName,
            "The current Manager image");
        if (!validManager) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(validManager).error());
        }
        auto requestedCli = siblingCliPath(
            platform.value().currentManagerImage.canonicalPath);
        if (!requestedCli) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(requestedCli).error());
        }
        auto cliIdentity =
            platformProbe_.executableIdentity(requestedCli.value(), context);
        if (!cliIdentity) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(cliIdentity).error());
        }
        auto validCli = validateExecutableIdentity(
            cliIdentity.value(), L"forge-conductor.exe",
            "The sibling Forge Conductor image");
        if (!validCli) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                std::move(validCli).error());
        }
        if (!equalWindowsPath(
                requestedCli.value(),
                cliIdentity.value().canonicalPath)) {
            return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
                integrityFailure(
                    "The Forge Conductor executable is not the exact sibling of the current Manager image."));
        }

        const std::uint64_t physicalMemoryBytes =
            platform.value().physicalMemoryBytes;
        const Domain::ResourceProfile resourceProfile =
            Domain::selectResourceProfile(physicalMemoryBytes);
        return Domain::Result<ManagerProcessEnvironmentSnapshot>::success(
            ManagerProcessEnvironmentSnapshot{
                std::move(dataRoot).value(),
                std::move(configurationRoot).value(),
                std::move(diagnosticsRoot).value(),
                std::move(exportRoot).value(),
                std::move(projectsRoot).value(),
                std::move(platform).value().currentManagerImage,
                std::move(cliIdentity).value(),
                physicalMemoryBytes,
                resourceProfile,
                Domain::budgetsForProfile(resourceProfile)});
    } catch (...) {
        return Domain::Result<ManagerProcessEnvironmentSnapshot>::failure(
            internalFailure(
                "The Manager process environment inspection failed safely."));
    }
}

Domain::Result<PreparedManagerProcessEnvironment>
ManagerProcessEnvironment::prepareAfterLease(
    const ManagerProcessEnvironmentSnapshot& inspectedSnapshot,
    InfrastructureWindows::WindowsManagerInstanceLease lease,
    const Domain::OperationContext& context) const noexcept
{
    try {
        if (!lease.owns()) {
            return Domain::Result<PreparedManagerProcessEnvironment>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::OwnershipConflict,
                    "Manager process directories cannot be prepared without the live instance lease."));
        }

        auto revalidated = inspect(context);
        if (!revalidated) {
            return Domain::Result<PreparedManagerProcessEnvironment>::failure(
                std::move(revalidated).error());
        }
        if (revalidated.value() != inspectedSnapshot) {
            return Domain::Result<PreparedManagerProcessEnvironment>::failure(
                integrityFailure(
                    "Manager process roots or executable identities changed between inspection and lease preparation."));
        }

        const std::array<const Domain::PathText*, RequiredDirectoryCount>
            directories{
                &revalidated.value().dataRoot(),
                &revalidated.value().configurationRoot(),
                &revalidated.value().diagnosticsRoot(),
                &revalidated.value().exportRoot(),
                &revalidated.value().projectsRoot()};
        for (const Domain::PathText* const directory : directories) {
            auto active = validateContext(
                context, "prepare the Manager process directory set");
            if (!active) {
                return Domain::Result<
                    PreparedManagerProcessEnvironment>::failure(
                    std::move(active).error());
            }
            auto prepared =
                platformProbe_.ensureRegularDirectory(*directory, context);
            if (!prepared) {
                return Domain::Result<
                    PreparedManagerProcessEnvironment>::failure(
                    std::move(prepared).error());
            }
        }

        return Domain::Result<PreparedManagerProcessEnvironment>::success(
            PreparedManagerProcessEnvironment{
                std::move(revalidated).value(), std::move(lease)});
    } catch (...) {
        return Domain::Result<PreparedManagerProcessEnvironment>::failure(
            internalFailure(
                "The Manager process environment preparation failed safely."));
    }
}

} // namespace ForgeConductor::Composition::Windows
