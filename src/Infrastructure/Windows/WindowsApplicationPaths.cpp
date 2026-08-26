#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"

#include "Detail/OperationContextGuard.h"
#include "Detail/UniqueCoTaskMemAllocation.h"
#include "Detail/UtfConversion.h"
#include "Detail/Win32Error.h"
#include "Detail/WindowsPathResolver.h"

#include <ShlObj.h>
#include <Windows.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr wchar_t EnvironmentOverrideName[] = L"FORGE_CONDUCTOR_HOME";
constexpr wchar_t ProductDirectoryName[] = L"Forge Conductor";
constexpr std::size_t MaximumEnvironmentValueCharacters = 32U * 1024U;

[[nodiscard]] Domain::Result<std::optional<std::wstring>> environmentOverride() noexcept
{
    try {
        std::array<wchar_t, MaximumEnvironmentValueCharacters> buffer{};
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetEnvironmentVariableW(EnvironmentOverrideName, buffer.data(),
                                                       static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            const DWORD nativeError = ::GetLastError();
            if (nativeError == ERROR_SUCCESS || nativeError == ERROR_ENVVAR_NOT_FOUND) {
                return Domain::Result<std::optional<std::wstring>>::success(std::nullopt);
            }
            return Domain::Result<std::optional<std::wstring>>::failure(Detail::makeWin32Error(
                "read the opt-in Forge Conductor home override", nativeError));
        }
        if (length >= buffer.size()) {
            return Domain::Result<std::optional<std::wstring>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "FORGE_CONDUCTOR_HOME exceeds the supported Windows path limit."));
        }
        return Domain::Result<std::optional<std::wstring>>::success(
            std::wstring{buffer.data(), static_cast<std::size_t>(length)});
    } catch (...) {
        return Domain::Result<std::optional<std::wstring>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The opt-in Forge Conductor home override could not be read."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText>
resolveWideRoot(const std::wstring_view value) noexcept
{
    auto utf8 = Detail::strictUtf16ToUtf8(value);
    if (!utf8) {
        return Domain::Result<Domain::PathText>::failure(std::move(utf8).error());
    }
    auto resolved = Detail::WindowsPathResolver::resolveAppOwnedRoot(utf8.value());
    if (!resolved) {
        return Domain::Result<Domain::PathText>::failure(std::move(resolved).error());
    }
    return Detail::WindowsPathResolver::toPathText(resolved.value());
}

[[nodiscard]] Domain::Result<Domain::PathText>
resolveDataRoot(const WindowsApplicationPathsOptions& options) noexcept
{
    try {
        if (options.explicitDataRoot.has_value()) {
            auto resolved =
                Detail::WindowsPathResolver::resolveAppOwnedRoot(options.explicitDataRoot->value());
            if (!resolved) {
                return Domain::Result<Domain::PathText>::failure(std::move(resolved).error());
            }
            return Detail::WindowsPathResolver::toPathText(resolved.value());
        }

        if (options.allowEnvironmentOverride) {
            auto overrideValue = environmentOverride();
            if (!overrideValue) {
                return Domain::Result<Domain::PathText>::failure(std::move(overrideValue).error());
            }
            if (overrideValue.value().has_value()) {
                return resolveWideRoot(overrideValue.value().value());
            }
        }

        PWSTR rawLocalAppData = nullptr;
        const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                                      nullptr, &rawLocalAppData);
        Detail::UniqueCoTaskMemAllocation<wchar_t> localAppData{rawLocalAppData};
        if (FAILED(result) || rawLocalAppData == nullptr || *rawLocalAppData == L'\0') {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "SHGetKnownFolderPath could not resolve FOLDERID_LocalAppData (HRESULT " +
                    std::to_string(static_cast<long>(result)) + ")."));
        }

        std::wstring root{rawLocalAppData};
        if (!root.empty() && root.back() != L'\\') {
            root.push_back(L'\\');
        }
        root.append(ProductDirectoryName);
        return resolveWideRoot(root);
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The Forge Conductor application data root could not be resolved."));
    }
}

} // namespace

WindowsApplicationPaths::WindowsApplicationPaths(WindowsApplicationPathsOptions options) noexcept
    : dataRoot_{resolveDataRoot(options)}
{
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::rootFor(const Domain::OperationContext& context,
                                 const char* const action) const noexcept
{
    try {
        auto contextValidation =
            Detail::validateOperationContext(context, std::chrono::steady_clock::now(), action);
        if (!contextValidation) {
            return Domain::Result<Domain::PathText>::failure(std::move(contextValidation).error());
        }
        if (!dataRoot_) {
            return Domain::Result<Domain::PathText>::failure(dataRoot_.error());
        }
        return Domain::Result<Domain::PathText>::success(dataRoot_.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The immutable application path could not be returned."));
    }
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::childFor(const Domain::OperationContext& context,
                                  const wchar_t* const relativePath,
                                  const char* const action) const noexcept
{
    try {
        auto root = rootFor(context, action);
        if (!root) {
            return root;
        }
        auto rootUtf16 = Detail::strictUtf8ToUtf16(root.value().value());
        if (!rootUtf16) {
            return Domain::Result<Domain::PathText>::failure(std::move(rootUtf16).error());
        }
        auto resolved = Detail::WindowsPathResolver::resolveAppOwnedChild(
            rootUtf16.value(), relativePath, Detail::MissingPathPolicy::AllowDescendants);
        if (!resolved) {
            return Domain::Result<Domain::PathText>::failure(std::move(resolved).error());
        }
        return Detail::WindowsPathResolver::toPathText(resolved.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The app-owned child path could not be returned."));
    }
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::dataRoot(const Domain::OperationContext& context) noexcept
{
    return rootFor(context, "resolve the application data root");
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::configurationRoot(const Domain::OperationContext& context) noexcept
{
    return childFor(context, L"config", "resolve the configuration root");
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::diagnosticsRoot(const Domain::OperationContext& context) noexcept
{
    return childFor(context, L"logs", "resolve the diagnostics root");
}

Domain::Result<Domain::PathText>
WindowsApplicationPaths::projectRoot(const Domain::ProjectId& projectId,
                                     const Domain::OperationContext& context) noexcept
{
    try {
        auto projectIdUtf16 = Detail::strictUtf8ToUtf16(projectId.value());
        if (!projectIdUtf16) {
            return Domain::Result<Domain::PathText>::failure(std::move(projectIdUtf16).error());
        }
        std::wstring relative{L"projects\\"};
        relative.append(projectIdUtf16.value());
        return childFor(context, relative.c_str(), "resolve the project data root");
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The project data root could not be returned."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
