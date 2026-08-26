#include "WindowsPathResolver.h"

#include "UniqueHandle.h"
#include "UniqueCoTaskMemAllocation.h"
#include "UtfConversion.h"
#include "Win32Error.h"

#include <ShlObj.h>
#include <Windows.h>
#include <appmodel.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail
{
namespace
{

constexpr std::size_t MaximumNativePathCharacters = 32U * 1024U;

[[nodiscard]] Domain::Result<std::wstring> pathFailure(const std::string_view code,
                                                       std::string message)
{
    return Domain::Result<std::wstring>::failure(Domain::makeError(code, std::move(message)));
}

[[nodiscard]] bool equalPath(const std::wstring_view left, const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
           ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool startsWithPath(const std::wstring_view value,
                                  const std::wstring_view prefix) noexcept
{
    if (value.size() < prefix.size() ||
        ::CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                               static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
    {
        return false;
    }
    if (value.size() == prefix.size() || prefix.back() == L'\\')
    {
        return true;
    }
    return value[prefix.size()] == L'\\';
}

[[nodiscard]] bool isForbiddenPathCharacter(const wchar_t value) noexcept
{
    return value < 0x20 || value == L'<' || value == L'>' || value == L'"' || value == L'|' ||
           value == L'?' || value == L'*' || value == L':';
}

[[nodiscard]] bool isReservedDeviceComponent(const std::wstring_view component) noexcept
{
    const std::size_t dot = component.find(L'.');
    const std::wstring_view base = component.substr(0, dot);
    if (equalPath(base, L"CON") || equalPath(base, L"PRN") || equalPath(base, L"AUX") ||
        equalPath(base, L"NUL") || equalPath(base, L"CONIN$") || equalPath(base, L"CONOUT$"))
    {
        return true;
    }
    return base.size() == 4U && base[3] >= L'1' && base[3] <= L'9' &&
           (equalPath(base.substr(0, 3), L"COM") || equalPath(base.substr(0, 3), L"LPT"));
}

[[nodiscard]] bool isValidComponent(const std::wstring_view component) noexcept
{
    if (component.empty() || component == L"." || component == L".." || component.back() == L' ' ||
        component.back() == L'.' || isReservedDeviceComponent(component))
    {
        return false;
    }
    return std::none_of(component.begin(), component.end(), isForbiddenPathCharacter);
}

[[nodiscard]] Domain::Result<void> validateRelativePath(const std::wstring_view value) noexcept
{
    try
    {
        if (value.empty() || value.size() > MaximumNativePathCharacters || value.front() == L'\\' ||
            value.front() == L'/' || value.back() == L'\\' ||
            value.find(L'/') != std::wstring_view::npos ||
            value.find(L'\0') != std::wstring_view::npos ||
            value.find(L':') != std::wstring_view::npos)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "An app-owned relative path has an unsafe or non-canonical form."));
        }

        std::size_t start = 0;
        while (start < value.size())
        {
            const std::size_t separator = value.find(L'\\', start);
            const std::size_t end = separator == std::wstring_view::npos ? value.size() : separator;
            if (!isValidComponent(value.substr(start, end - start)))
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                      "An app-owned relative path contains a reserved component."));
            }
            if (separator == std::wstring_view::npos)
            {
                break;
            }
            start = separator + 1;
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The relative path could not be validated."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> normalizeAbsolutePath(
    const std::wstring_view value) noexcept
{
    try
    {
        if (value.starts_with(L"\\\\") || value.starts_with(L"//") ||
            value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\"))
        {
            return pathFailure(Domain::ErrorCodes::PathOutsideAuthority,
                               "UNC and device paths are not permitted for app-owned storage.");
        }
        if (value.size() < 3 || value.size() > MaximumNativePathCharacters ||
            value.find(L'\0') != std::wstring_view::npos ||
            value.find(L'/') != std::wstring_view::npos || value[1] != L':' || value[2] != L'\\' ||
            std::iswalpha(value[0]) == 0 || (value.size() > 3 && value.back() == L'\\'))
        {
            return pathFailure(Domain::ErrorCodes::InvalidRequest,
                               "An app-owned path must be a canonical absolute drive path.");
        }

        std::size_t start = 3;
        while (start < value.size())
        {
            const std::size_t separator = value.find(L'\\', start);
            const std::size_t end = separator == std::wstring_view::npos ? value.size() : separator;
            if (!isValidComponent(value.substr(start, end - start)))
            {
                return pathFailure(Domain::ErrorCodes::InvalidRequest,
                                   "An app-owned path contains an alternate stream or "
                                   "reserved component.");
            }
            if (separator == std::wstring_view::npos)
            {
                break;
            }
            start = separator + 1;
        }

        const std::wstring input{value};
        const DWORD required = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
        if (required == 0)
        {
            return Domain::Result<std::wstring>::failure(
                makeWin32Error("canonicalize an app-owned path", ::GetLastError()));
        }
        if (required > MaximumNativePathCharacters)
        {
            return pathFailure(Domain::ErrorCodes::PayloadTooLarge,
                               "The canonical app-owned path exceeds its Windows length limit.");
        }

        std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
        const DWORD written = ::GetFullPathNameW(input.c_str(), required, buffer.data(), nullptr);
        if (written == 0 || written >= required)
        {
            return Domain::Result<std::wstring>::failure(
                makeWin32Error("canonicalize an app-owned path", ::GetLastError()));
        }

        std::wstring canonical{buffer.data(), static_cast<std::size_t>(written)};
        if (!equalPath(value, canonical))
        {
            return pathFailure(Domain::ErrorCodes::InvalidRequest,
                               "The app-owned path changes under Windows lexical normalization.");
        }
        return Domain::Result<std::wstring>::success(std::move(canonical));
    }
    catch (...)
    {
        return pathFailure(Domain::ErrorCodes::InternalFailure,
                           "The app-owned path could not be canonicalized.");
    }
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view path)
{
    std::wstring result{L"\\\\?\\"};
    result.append(path);
    return result;
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value)
{
    if (value.starts_with(L"\\\\?\\UNC\\"))
    {
        value.erase(0, 7);
        value.insert(value.begin(), L'\\');
        return value;
    }
    if (value.starts_with(L"\\\\?\\"))
    {
        value.erase(0, 4);
    }
    return value;
}

[[nodiscard]] bool isExpectedPackagedLocalAppDataRedirect(
    const std::wstring_view requested,
    const std::wstring_view opened) noexcept
{
    try {
        PWSTR rawLocalAppData{};
        const HRESULT folderResult = ::SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
            &rawLocalAppData);
        UniqueCoTaskMemAllocation<wchar_t> localAppDataOwner{
            rawLocalAppData};
        if (FAILED(folderResult) || rawLocalAppData == nullptr ||
            *rawLocalAppData == L'\0') {
            return false;
        }
        std::wstring localAppData{rawLocalAppData};
        while (localAppData.size() > 3U &&
               localAppData.back() == L'\\') {
            localAppData.pop_back();
        }
        if (!equalPath(requested, localAppData) &&
            !startsWithPath(requested, localAppData)) {
            return false;
        }

        std::wstring packagePrefix{localAppData};
        packagePrefix.append(L"\\Packages\\");
        if (opened.size() <= packagePrefix.size() ||
            ::CompareStringOrdinal(
                opened.data(), static_cast<int>(packagePrefix.size()),
                packagePrefix.data(), static_cast<int>(packagePrefix.size()),
                TRUE) != CSTR_EQUAL) {
            return false;
        }
        const auto familyEnd = opened.find(L'\\', packagePrefix.size());
        if (familyEnd == std::wstring_view::npos) {
            return false;
        }
        const auto openedFamily = opened.substr(
            packagePrefix.size(), familyEnd - packagePrefix.size());
        if (openedFamily.size() > PACKAGE_FAMILY_NAME_MAX_LENGTH ||
            !isValidComponent(openedFamily)) {
            return false;
        }

        UINT32 familyCharacters{};
        const LONG lengthResult = ::GetCurrentPackageFamilyName(
            &familyCharacters, nullptr);
        if (lengthResult == ERROR_INSUFFICIENT_BUFFER &&
            familyCharacters > 1U &&
            familyCharacters <= PACKAGE_FAMILY_NAME_MAX_LENGTH + 1U) {
            std::vector<wchar_t> family(familyCharacters, L'\0');
            if (::GetCurrentPackageFamilyName(
                    &familyCharacters, family.data()) != ERROR_SUCCESS ||
                !equalPath(
                    openedFamily,
                    std::wstring_view{
                        family.data(),
                        static_cast<std::size_t>(familyCharacters - 1U)})) {
                return false;
            }
        } else if (lengthResult != APPMODEL_ERROR_NO_PACKAGE) {
            return false;
        }

        std::wstring expected{packagePrefix};
        expected.append(openedFamily);
        expected.append(L"\\LocalCache\\Local");
        expected.append(requested.substr(localAppData.size()));
        return equalPath(expected, opened);
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] Domain::Result<void> verifyOpenedHandle(const HANDLE handle,
                                                      const std::wstring_view path,
                                                      const bool mustBeDirectory) noexcept
{
    try
    {
        FILE_ATTRIBUTE_TAG_INFO tagInfo{};
        if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                           sizeof(tagInfo)) == FALSE)
        {
            return Domain::Result<void>::failure(
                makeWin32Error("inspect an app-owned path", ::GetLastError()));
        }
        if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                  "App-owned storage cannot traverse a reparse point."));
        }
        if (mustBeDirectory && (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "An intermediate app-owned path component is not a directory."));
        }
        if (mustBeDirectory)
        {
            FILE_CASE_SENSITIVE_INFO caseSensitivity{};
            if (::GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &caseSensitivity,
                                               sizeof(caseSensitivity)) == FALSE)
            {
                return Domain::Result<void>::failure(makeWin32Error(
                    "inspect app-owned directory case-sensitivity", ::GetLastError()));
            }
            auto supportedCasePolicy =
                WindowsPathResolver::validateDirectoryCaseSensitivityFlags(caseSensitivity.Flags);
            if (!supportedCasePolicy)
            {
                return supportedCasePolicy;
            }
        }

        const DWORD required =
            ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0 || required > MaximumNativePathCharacters + 4U)
        {
            return Domain::Result<void>::failure(
                makeWin32Error("resolve an opened app-owned path", ::GetLastError()));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written =
            ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0 || written >= buffer.size())
        {
            return Domain::Result<void>::failure(
                makeWin32Error("resolve an opened app-owned path", ::GetLastError()));
        }
        std::wstring opened =
            withoutExtendedPrefix(std::wstring{buffer.data(), static_cast<std::size_t>(written)});
        if (!equalPath(path, opened) &&
            !isExpectedPackagedLocalAppDataRedirect(path, opened))
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                  "The opened path differs from the requested canonical path."));
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The opened app-owned path could not be validated."));
    }
}

[[nodiscard]] Domain::Result<void> verifyOpenedPath(const std::wstring_view path,
                                                    const bool mustBeDirectory) noexcept
{
    try
    {
        const std::wstring nativePath = extendedPath(path);
        UniqueHandle handle{::CreateFileW(
            nativePath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle)
        {
            return Domain::Result<void>::failure(makeWin32Error(
                "open an app-owned path for canonical validation", ::GetLastError()));
        }
        return verifyOpenedHandle(handle.get(), path, mustBeDirectory);
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The opened app-owned path could not be validated."));
    }
}

[[nodiscard]] Domain::Result<UniqueHandle> openDirectoryAnchor(
    const std::wstring_view path, const bool allowChildFileCreation) noexcept
{
    try
    {
        const std::wstring nativePath = extendedPath(path);
        DWORD desiredAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
        if (allowChildFileCreation)
        {
            desiredAccess |= FILE_ADD_FILE | FILE_DELETE_CHILD;
        }
        UniqueHandle handle{::CreateFileW(
            nativePath.c_str(), desiredAccess, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle)
        {
            return Domain::Result<UniqueHandle>::failure(makeWin32Error(
                "anchor an app-owned directory against replacement", ::GetLastError()));
        }
        auto verified = verifyOpenedHandle(handle.get(), path, true);
        if (!verified)
        {
            return Domain::Result<UniqueHandle>::failure(std::move(verified).error());
        }
        return Domain::Result<UniqueHandle>::success(std::move(handle));
    }
    catch (...)
    {
        return Domain::Result<UniqueHandle>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The app-owned directory could not be anchored."));
    }
}

[[nodiscard]] Domain::Result<std::vector<UniqueHandle>> anchorExistingParentDirectories(
    const std::wstring_view path, const bool allowChildFileCreation) noexcept
{
    try
    {
        const std::size_t finalSeparator = path.find_last_of(L'\\');
        if (finalSeparator == std::wstring_view::npos || finalSeparator < 2U)
        {
            return Domain::Result<std::vector<UniqueHandle>>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "The authorized path has no canonical parent directory."));
        }

        std::vector<UniqueHandle> anchors;
        std::size_t componentEnd = 3U;
        for (;;)
        {
            auto anchor =
                openDirectoryAnchor(path.substr(0, componentEnd),
                                    allowChildFileCreation && componentEnd == finalSeparator);
            if (!anchor)
            {
                return Domain::Result<std::vector<UniqueHandle>>::failure(
                    std::move(anchor).error());
            }
            anchors.push_back(std::move(anchor).value());
            if (componentEnd == finalSeparator)
            {
                break;
            }
            componentEnd = path.find(L'\\', componentEnd + 1U);
            if (componentEnd == std::wstring_view::npos || componentEnd > finalSeparator)
            {
                componentEnd = finalSeparator;
            }
        }
        return Domain::Result<std::vector<UniqueHandle>>::success(std::move(anchors));
    }
    catch (...)
    {
        return Domain::Result<std::vector<UniqueHandle>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The authorized path ancestry could not be anchored."));
    }
}

[[nodiscard]] bool isMissingError(const DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] Domain::Result<void> verifyExistingAncestry(const std::wstring_view path,
                                                          const MissingPathPolicy policy) noexcept
{
    try
    {
        std::vector<std::wstring> ancestors;
        ancestors.emplace_back(path.substr(0, 3));
        std::size_t end = 3;
        while (end < path.size())
        {
            const std::size_t separator = path.find(L'\\', end);
            const std::size_t componentEnd =
                separator == std::wstring_view::npos ? path.size() : separator;
            ancestors.emplace_back(path.substr(0, componentEnd));
            if (separator == std::wstring_view::npos)
            {
                break;
            }
            end = separator + 1;
        }

        for (std::size_t index = 0; index < ancestors.size(); ++index)
        {
            const std::wstring nativePath = extendedPath(ancestors[index]);
            const DWORD attributes = ::GetFileAttributesW(nativePath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const DWORD nativeError = ::GetLastError();
                const bool finalComponent = index + 1U == ancestors.size();
                if (isMissingError(nativeError) &&
                    (policy == MissingPathPolicy::AllowDescendants ||
                     (policy == MissingPathPolicy::AllowLeaf && finalComponent)))
                {
                    return Domain::Result<void>::success();
                }
                if (isMissingError(nativeError))
                {
                    return Domain::Result<void>::failure(
                        Domain::makeError(Domain::ErrorCodes::RecordNotFound,
                                          "An app-owned path or required parent does not exist."));
                }
                return Domain::Result<void>::failure(
                    makeWin32Error("inspect an app-owned path ancestor", nativeError));
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                      "App-owned storage cannot traverse a reparse point."));
            }

            const bool mustBeDirectory = index + 1U < ancestors.size();
            if (index != 0)
            {
                auto opened = verifyOpenedPath(ancestors[index], mustBeDirectory);
                if (!opened)
                {
                    return opened;
                }
            }
            else if (mustBeDirectory && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                      "The app-owned path drive root is not a directory."));
            }
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The app-owned path ancestry could not be validated."));
    }
}

} // namespace

Domain::Result<void> WindowsPathResolver::validateDirectoryCaseSensitivityFlags(
    const std::uint32_t flags) noexcept
{
    if ((flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0U)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "Case-sensitive directories are not supported for app-owned storage."));
    }
    return Domain::Result<void>::success();
}

AnchoredAuthorizedPath::AnchoredAuthorizedPath(std::wstring canonicalPath,
                                               std::vector<UniqueHandle> directoryAnchors) noexcept
    : canonicalPath_{std::move(canonicalPath)}, directoryAnchors_{std::move(directoryAnchors)}
{
}

HANDLE AnchoredAuthorizedPath::parentDirectoryHandle() const noexcept
{
    return directoryAnchors_.empty() ? nullptr : directoryAnchors_.back().get();
}

Domain::Result<void> AnchoredAuthorizedPath::revalidateDirectoryAnchors() const noexcept
{
    try
    {
        const std::size_t finalSeparator = canonicalPath_.find_last_of(L'\\');
        if (finalSeparator == std::wstring::npos || finalSeparator < 2U ||
            directoryAnchors_.empty())
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "The anchored path has no retained parent directory."));
        }

        std::size_t anchorIndex{};
        std::size_t componentEnd = 3U;
        for (;;)
        {
            if (anchorIndex >= directoryAnchors_.size())
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "The anchored path ancestry is incomplete."));
            }
            auto verified = verifyOpenedHandle(directoryAnchors_[anchorIndex].get(),
                                               canonicalPath_.substr(0U, componentEnd), true);
            if (!verified)
            {
                return verified;
            }
            ++anchorIndex;
            if (componentEnd == finalSeparator)
            {
                break;
            }
            componentEnd = canonicalPath_.find(L'\\', componentEnd + 1U);
            if (componentEnd == std::wstring::npos || componentEnd > finalSeparator)
            {
                componentEnd = finalSeparator;
            }
        }
        if (anchorIndex != directoryAnchors_.size())
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "The anchored path ancestry contains unexpected handles."));
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The anchored path ancestry could not be revalidated."));
    }
}

Domain::Result<std::wstring> WindowsPathResolver::resolveAppOwnedRoot(
    const std::string_view utf8Path) noexcept
{
    try
    {
        auto converted = strictUtf8ToUtf16(utf8Path);
        if (!converted)
        {
            return Domain::Result<std::wstring>::failure(std::move(converted).error());
        }
        auto normalized = normalizeAbsolutePath(converted.value());
        if (!normalized)
        {
            return normalized;
        }
        if (normalized.value().size() == 3U)
        {
            return pathFailure(
                Domain::ErrorCodes::InvalidRequest,
                "A drive root is too broad to own Forge Conductor application data.");
        }
        auto ancestry =
            verifyExistingAncestry(normalized.value(), MissingPathPolicy::AllowDescendants);
        if (!ancestry)
        {
            return Domain::Result<std::wstring>::failure(std::move(ancestry).error());
        }
        return normalized;
    }
    catch (...)
    {
        return pathFailure(Domain::ErrorCodes::InternalFailure,
                           "The app-owned root could not be resolved.");
    }
}

Domain::Result<std::wstring> WindowsPathResolver::resolveAppOwnedChild(
    const std::wstring_view canonicalRoot, const std::wstring_view relativePath,
    const MissingPathPolicy policy) noexcept
{
    try
    {
        auto root = normalizeAbsolutePath(canonicalRoot);
        if (!root)
        {
            return root;
        }
        auto rootAncestry =
            verifyExistingAncestry(root.value(), MissingPathPolicy::AllowDescendants);
        if (!rootAncestry)
        {
            return Domain::Result<std::wstring>::failure(std::move(rootAncestry).error());
        }
        auto relativeValidation = validateRelativePath(relativePath);
        if (!relativeValidation)
        {
            return Domain::Result<std::wstring>::failure(std::move(relativeValidation).error());
        }

        std::wstring joined = root.value();
        if (joined.back() != L'\\')
        {
            joined.push_back(L'\\');
        }
        joined.append(relativePath);
        auto normalized = normalizeAbsolutePath(joined);
        if (!normalized)
        {
            return normalized;
        }
        if (!startsWithPath(normalized.value(), root.value()))
        {
            return pathFailure(Domain::ErrorCodes::PathOutsideAuthority,
                               "The app-owned child escaped its canonical root.");
        }

        auto ancestry = verifyExistingAncestry(normalized.value(), policy);
        if (!ancestry)
        {
            return Domain::Result<std::wstring>::failure(std::move(ancestry).error());
        }
        return normalized;
    }
    catch (...)
    {
        return pathFailure(Domain::ErrorCodes::InternalFailure,
                           "The app-owned child path could not be resolved.");
    }
}

Domain::Result<std::wstring> WindowsPathResolver::resolveAuthorizedPath(
    const Contracts::AuthorizedPath &path, const Domain::FileAccess requiredAccess,
    const MissingPathPolicy policy) noexcept
{
    try
    {
        if (path.access() != requiredAccess)
        {
            return pathFailure(Domain::ErrorCodes::Unauthorized,
                               "The authorized path does not grant the required access mode.");
        }

        auto root = resolveAppOwnedRoot(path.authorityRoot().value());
        if (!root)
        {
            return root;
        }
        auto targetUtf16 = strictUtf8ToUtf16(path.canonicalPath().value());
        if (!targetUtf16)
        {
            return Domain::Result<std::wstring>::failure(std::move(targetUtf16).error());
        }
        auto target = normalizeAbsolutePath(targetUtf16.value());
        if (!target)
        {
            return target;
        }
        if (!startsWithPath(target.value(), root.value()))
        {
            return pathFailure(Domain::ErrorCodes::PathOutsideAuthority,
                               "The authorized path is outside its canonical authority root.");
        }

        auto ancestry = verifyExistingAncestry(target.value(), policy);
        if (!ancestry)
        {
            return Domain::Result<std::wstring>::failure(std::move(ancestry).error());
        }
        return target;
    }
    catch (...)
    {
        return pathFailure(Domain::ErrorCodes::InternalFailure,
                           "The authorized Windows path could not be resolved.");
    }
}

Domain::Result<AnchoredAuthorizedPath> WindowsPathResolver::resolveAnchoredAuthorizedPath(
    const Contracts::AuthorizedPath &path, const Domain::FileAccess requiredAccess,
    const MissingPathPolicy policy) noexcept
{
    try
    {
        auto resolved = resolveAuthorizedPath(path, requiredAccess, policy);
        if (!resolved)
        {
            return Domain::Result<AnchoredAuthorizedPath>::failure(std::move(resolved).error());
        }
        const bool allowChildFileCreation = requiredAccess == Domain::FileAccess::Write ||
                                            requiredAccess == Domain::FileAccess::Create;
        auto anchors = anchorExistingParentDirectories(resolved.value(), allowChildFileCreation);
        if (!anchors)
        {
            return Domain::Result<AnchoredAuthorizedPath>::failure(std::move(anchors).error());
        }

        // Revalidate the leaf while every parent name is pinned. This closes
        // the validation-to-use window for a parent junction substitution.
        auto ancestry = verifyExistingAncestry(resolved.value(), policy);
        if (!ancestry)
        {
            return Domain::Result<AnchoredAuthorizedPath>::failure(std::move(ancestry).error());
        }
        return Domain::Result<AnchoredAuthorizedPath>::success(
            AnchoredAuthorizedPath{std::move(resolved).value(), std::move(anchors).value()});
    }
    catch (...)
    {
        return Domain::Result<AnchoredAuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The authorized Windows path could not be anchored."));
    }
}

Domain::Result<Domain::PathText> WindowsPathResolver::toPathText(
    const std::wstring_view canonicalPath) noexcept
{
    try
    {
        auto converted = strictUtf16ToUtf8(canonicalPath);
        if (!converted)
        {
            return Domain::Result<Domain::PathText>::failure(std::move(converted).error());
        }
        return Domain::PathText::create(converted.value());
    }
    catch (...)
    {
        return Domain::Result<Domain::PathText>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The canonical Windows path could not be represented as UTF-8."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
