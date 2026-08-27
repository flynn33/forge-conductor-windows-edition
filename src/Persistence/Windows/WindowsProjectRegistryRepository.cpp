#include "ForgeConductor/Persistence/Windows/WindowsProjectRegistryRepository.h"

#include "Infrastructure/Windows/Detail/BoundedSerialExecutor.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/RelativeFileOperations.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
namespace {

namespace InfrastructureDetail = Infrastructure::Windows::Detail;
using Json = nlohmann::json;
using UniqueHandle = InfrastructureDetail::UniqueHandle;

constexpr auto RegistryLockMaximumWait = std::chrono::seconds{3};
constexpr auto RegistryLockRetrySlice = std::chrono::milliseconds{10};
constexpr std::size_t MaximumDisplayNameBytes = 512U;
constexpr std::size_t MaximumTimestampBytes = 32U;
constexpr std::size_t MaximumNativePathCharacters = 32U * 1024U;

class RegistryDocumentException final : public std::runtime_error {
public:
    RegistryDocumentException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[noreturn]] void rejectDocument(
    const std::string_view code,
    std::string message)
{
    throw RegistryDocumentException{std::string{code}, std::move(message)};
}

[[nodiscard]] std::string trimAscii(std::string value);
[[nodiscard]] int ordinalCompare(
    std::wstring_view left,
    std::wstring_view right,
    bool ignoreCase) noexcept;
[[nodiscard]] bool ordinalEqualIgnoreCase(
    std::wstring_view left,
    std::wstring_view right) noexcept;

[[nodiscard]] bool reservedDosComponent(const std::wstring_view component) noexcept
{
    auto base = component.substr(0U, component.find(L'.'));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) {
        base.remove_suffix(1U);
    }
    const auto equals = [base](const std::wstring_view candidate) noexcept {
        return ordinalEqualIgnoreCase(base, candidate);
    };
    if (equals(L"CON") || equals(L"PRN") || equals(L"AUX") || equals(L"NUL") ||
        equals(L"CONIN$") || equals(L"CONOUT$") || equals(L"CLOCK$")) {
        return true;
    }
    if (base.size() != 4U) {
        return false;
    }
    const bool numbered =
        (base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' ||
        base[3] == L'\u00b2' || base[3] == L'\u00b3';
    return numbered &&
           (ordinalEqualIgnoreCase(base.substr(0U, 3U), L"COM") ||
            ordinalEqualIgnoreCase(base.substr(0U, 3U), L"LPT"));
}

[[nodiscard]] Domain::Result<Domain::PathText> normalizeLocalAliasLexically(
    const Domain::PathText& alias) noexcept
{
    try {
        auto utf8 = trimAscii(alias.value());
        while (utf8.size() > 3U && utf8.ends_with('\\')) {
            utf8.pop_back();
        }
        auto converted = InfrastructureDetail::strictUtf8ToUtf16(utf8);
        if (!converted) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project alias is not valid UTF-8."));
        }
        const auto& input = converted.value();
        if (input.size() <= 3U || input.size() > MaximumNativePathCharacters ||
            input.starts_with(L"\\\\") || input.starts_with(L"//") ||
            input.starts_with(L"\\\\?\\") || input.starts_with(L"\\\\.\\") ||
            input.find(L'/') != std::wstring::npos || input[1] != L':' ||
            input[2] != L'\\' || std::iswalpha(input[0]) == 0 ||
            input.find(L':', 2U) != std::wstring::npos) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A project alias must be a canonical local-drive path without a device, UNC, or alternate-stream form."));
        }

        std::size_t componentStart = 3U;
        while (componentStart < input.size()) {
            const auto separator = input.find(L'\\', componentStart);
            const auto componentEnd = separator == std::wstring::npos
                                          ? input.size()
                                          : separator;
            const auto component = input.substr(
                componentStart, componentEnd - componentStart);
            const bool invalidCharacter = std::any_of(
                component.begin(), component.end(), [](const wchar_t character) noexcept {
                    return character < 0x20 || character == L':' || character == L'<' ||
                           character == L'>' || character == L'"' || character == L'|' ||
                           character == L'?' || character == L'*';
                });
            if (component.empty() || component == L"." || component == L".." ||
                component.back() == L' ' || component.back() == L'.' ||
                invalidCharacter || reservedDosComponent(component)) {
                return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The project alias contains a reserved or ambiguous path component."));
            }
            if (separator == std::wstring::npos) {
                break;
            }
            componentStart = separator + 1U;
        }

        const DWORD required = ::GetFullPathNameW(input.c_str(), 0U, nullptr, nullptr);
        if (required == 0U || required > MaximumNativePathCharacters) {
            return Domain::Result<Domain::PathText>::failure(
                InfrastructureDetail::makeWin32Error(
                    "normalize a detached project alias", ::GetLastError(),
                    Domain::ErrorCodes::InvalidRequest));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
        const DWORD written = ::GetFullPathNameW(
            input.c_str(), required, buffer.data(), nullptr);
        if (written == 0U || written >= required) {
            return Domain::Result<Domain::PathText>::failure(
                InfrastructureDetail::makeWin32Error(
                    "normalize a detached project alias", ::GetLastError(),
                    Domain::ErrorCodes::InvalidRequest));
        }
        std::wstring normalized{buffer.data(), static_cast<std::size_t>(written)};
        if (!ordinalEqualIgnoreCase(input, normalized)) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project alias changes under Windows lexical normalization."));
        }
        auto result = InfrastructureDetail::WindowsPathResolver::toPathText(normalized);
        if (!result) {
            return Domain::Result<Domain::PathText>::failure(std::move(result).error());
        }
        return result;
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project alias could not be normalized."));
    }
}

[[nodiscard]] bool sameAlias(
    const Domain::PathText& left,
    const Domain::PathText& right) noexcept
{
    auto leftWide = InfrastructureDetail::strictUtf8ToUtf16(left.value());
    auto rightWide = InfrastructureDetail::strictUtf8ToUtf16(right.value());
    return leftWide && rightWide &&
           ordinalEqualIgnoreCase(leftWide.value(), rightWide.value());
}

[[nodiscard]] bool aliasLess(
    const Domain::PathText& left,
    const Domain::PathText& right) noexcept
{
    auto leftWide = InfrastructureDetail::strictUtf8ToUtf16(left.value());
    auto rightWide = InfrastructureDetail::strictUtf8ToUtf16(right.value());
    if (!leftWide || !rightWide) {
        return left.value() < right.value();
    }
    const int insensitive = ordinalCompare(leftWide.value(), rightWide.value(), true);
    if (insensitive != 0) {
        return insensitive < 0;
    }
    return ordinalCompare(leftWide.value(), rightWide.value(), false) < 0;
}

[[nodiscard]] Domain::Result<std::string> formatUtcTimestamp(
    const Contracts::IClock& clock) noexcept
{
    try {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(clock.utcNow());
        std::tm utc{};
        if (::gmtime_s(&utc, &seconds) != 0) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The project-registry timestamp is outside the supported UTC range."));
        }
        std::array<char, 21U> buffer{};
        const int written = std::snprintf(
            buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
            utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);
        if (written != 20) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The project-registry timestamp could not be formatted."));
        }
        return Domain::Result<std::string>::success(
            std::string{buffer.data(), static_cast<std::size_t>(written)});
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-registry timestamp could not be created."));
    }
}

[[nodiscard]] bool validTimestamp(const std::string_view value) noexcept
{
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index == 4U || index == 7U || index == 10U || index == 13U ||
            index == 16U || index == 19U) {
            continue;
        }
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

struct RegistryEntry final {
    Domain::ProjectId id;
    std::string displayName;
    std::optional<std::string> repositoryIdentity;
    std::vector<Domain::PathText> aliases;
    std::string createdAt;
    std::string updatedAt;
};

struct RegistryDocument final {
    std::vector<RegistryEntry> projects;
};

[[nodiscard]] Domain::ProjectMemoryDescriptor descriptorFor(const RegistryEntry& entry)
{
    return Domain::ProjectMemoryDescriptor{
        entry.id, entry.displayName, entry.repositoryIdentity, entry.aliases};
}

[[nodiscard]] bool projectLess(
    const RegistryEntry& left,
    const RegistryEntry& right) noexcept
{
    return left.id.value() < right.id.value();
}

void sortDocument(RegistryDocument& document)
{
    for (auto& entry : document.projects) {
        std::sort(entry.aliases.begin(), entry.aliases.end(), aliasLess);
    }
    std::sort(document.projects.begin(), document.projects.end(), projectLess);
}

[[nodiscard]] std::string trimAscii(std::string value)
{
    const auto whitespace = [](const unsigned char character) noexcept {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n' || character == '\f' || character == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    if (first >= last) {
        return {};
    }
    return std::string{first, last};
}

[[nodiscard]] std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character + ('a' - 'A'))
                   : static_cast<char>(character);
    });
    return value;
}

[[nodiscard]] bool containsControlCharacter(const std::string_view value) noexcept
{
    return std::any_of(value.begin(), value.end(), [](const unsigned char character) noexcept {
        return character < 0x20U || character == 0x7fU;
    });
}

[[nodiscard]] Domain::Result<std::optional<std::string>> normalizeOptionalText(
    const std::optional<std::string>& value,
    const std::size_t maximumBytes,
    const std::string_view field) noexcept
{
    try {
        if (!value) {
            return Domain::Result<std::optional<std::string>>::success(std::nullopt);
        }
        auto normalized = trimAscii(*value);
        if (normalized.empty()) {
            return Domain::Result<std::optional<std::string>>::success(std::nullopt);
        }
        if (normalized.size() > maximumBytes || containsControlCharacter(normalized)) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{field} + " is invalid or exceeds its UTF-8 byte limit."));
        }
        auto unicode = InfrastructureDetail::strictUtf8ToUtf16(normalized);
        if (!unicode) {
            return Domain::Result<std::optional<std::string>>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  std::string{field} + " is not valid UTF-8."));
        }
        return Domain::Result<std::optional<std::string>>::success(
            std::optional<std::string>{std::move(normalized)});
    } catch (...) {
        return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{field} + " could not be normalized."));
    }
}

[[nodiscard]] Domain::Result<std::optional<std::string>> normalizeRepositoryIdentity(
    const std::optional<std::string>& value) noexcept
{
    auto normalized = normalizeOptionalText(
        value,
        WindowsProjectRegistryRepository::MaximumRepositoryIdentityBytes,
        "repository_identity");
    if (!normalized || !normalized.value()) {
        return normalized;
    }

    auto candidate = std::move(normalized).value().value();
    if (candidate.size() >= 4U && lowerAscii(candidate.substr(0U, 4U)) == "git:") {
        auto digestText = lowerAscii(candidate.substr(4U));
        auto digest = Domain::Sha256Digest::parse(digestText);
        if (!digest) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A git repository identity must contain a canonical SHA-256 digest."));
        }
        candidate = "git:" + digest.value().value();
    }
    return Domain::Result<std::optional<std::string>>::success(
        std::optional<std::string>{std::move(candidate)});
}

[[nodiscard]] int ordinalCompare(
    const std::wstring_view left,
    const std::wstring_view right,
    const bool ignoreCase) noexcept
{
    if (left.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
        right.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        const int compared = ::CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), ignoreCase ? TRUE : FALSE);
        if (compared == CSTR_LESS_THAN) {
            return -1;
        }
        if (compared == CSTR_GREATER_THAN) {
            return 1;
        }
        if (compared == CSTR_EQUAL) {
            return 0;
        }
    }
    if (left < right) {
        return -1;
    }
    return left == right ? 0 : 1;
}

[[nodiscard]] bool ordinalEqualIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return ordinalCompare(left, right, true) == 0;
}

[[nodiscard]] std::wstring removeExtendedPrefix(std::wstring value)
{
    if (value.starts_with(L"\\\\?\\UNC\\")) {
        value.erase(0U, 7U);
        value.insert(value.begin(), L'\\');
        return value;
    }
    if (value.starts_with(L"\\\\?\\")) {
        value.erase(0U, 4U);
    }
    return value;
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view value)
{
    std::wstring result{L"\\\\?\\"};
    result.append(value);
    return result;
}

[[nodiscard]] Domain::Result<std::wstring> finalPathForHandle(
    const HANDLE handle,
    const std::string_view action) noexcept
{
    try {
        const DWORD required = ::GetFinalPathNameByHandleW(
            handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U || required > MaximumNativePathCharacters + 4U) {
            return Domain::Result<std::wstring>::failure(
                InfrastructureDetail::makeWin32Error(action, ::GetLastError(),
                                                     Domain::ErrorCodes::PathOutsideAuthority));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<std::wstring>::failure(
                InfrastructureDetail::makeWin32Error(action, ::GetLastError(),
                                                     Domain::ErrorCodes::PathOutsideAuthority));
        }
        return Domain::Result<std::wstring>::success(removeExtendedPrefix(
            std::wstring{buffer.data(), static_cast<std::size_t>(written)}));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "An opened registry path could not be canonicalized."));
    }
}

[[nodiscard]] Domain::Result<void> inspectDirectoryHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath,
    const std::string_view action) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(
            InfrastructureDetail::makeWin32Error(action, ::GetLastError(),
                                                 Domain::ErrorCodes::PathOutsideAuthority));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "A project path component is a reparse point or is not a directory."));
    }

    FILE_CASE_SENSITIVE_INFO caseSensitivity{};
    if (::GetFileInformationByHandleEx(
            handle, FileCaseSensitiveInfo, &caseSensitivity,
            sizeof(caseSensitivity)) == FALSE) {
        return Domain::Result<void>::failure(
            InfrastructureDetail::makeWin32Error(
                "inspect project-directory case sensitivity", ::GetLastError(),
                Domain::ErrorCodes::PathOutsideAuthority));
    }
    auto supported = InfrastructureDetail::WindowsPathResolver::
        validateDirectoryCaseSensitivityFlags(caseSensitivity.Flags);
    if (!supported) {
        return supported;
    }

    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<void>::failure(
            InfrastructureDetail::makeWin32Error(action, ::GetLastError(),
                                                 Domain::ErrorCodes::PathOutsideAuthority));
    }
    if (standard.DeletePending != FALSE) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "A project path component is pending deletion."));
    }

    auto finalPath = finalPathForHandle(handle, action);
    if (!finalPath) {
        return Domain::Result<void>::failure(std::move(finalPath).error());
    }
    if (!ordinalEqualIgnoreCase(finalPath.value(), expectedPath) &&
        !InfrastructureDetail::WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, finalPath.value())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The opened project path differs from its canonical local path."));
    }
    return Domain::Result<void>::success();
}

struct CanonicalProjectDirectory final {
    Domain::PathText path;
    std::wstring nativePath;
    UniqueHandle handle;
};

[[nodiscard]] Domain::Result<CanonicalProjectDirectory> canonicalProjectDirectory(
    const Domain::PathText& requestedPath) noexcept
{
    try {
        auto text = trimAscii(requestedPath.value());
        while (text.size() > 3U && (text.ends_with('\\') || text.ends_with('/'))) {
            text.pop_back();
        }
        if (text.empty()) {
            return Domain::Result<CanonicalProjectDirectory>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "project_path is required."));
        }

        auto lexical = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(text);
        if (!lexical) {
            auto error = std::move(lexical).error();
            if (error.code == Domain::ErrorCodes::RecordNotFound) {
                error.code = std::string{Domain::ErrorCodes::InvalidRequest};
                error.message = "project_path must name an existing local directory.";
            }
            return Domain::Result<CanonicalProjectDirectory>::failure(std::move(error));
        }

        const auto native = extendedPath(lexical.value());
        UniqueHandle handle{::CreateFileW(
            native.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle) {
            const DWORD nativeError = ::GetLastError();
            if (nativeError == ERROR_FILE_NOT_FOUND || nativeError == ERROR_PATH_NOT_FOUND ||
                nativeError == ERROR_DIRECTORY) {
                return Domain::Result<CanonicalProjectDirectory>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "project_path must name an existing local directory."));
            }
            return Domain::Result<CanonicalProjectDirectory>::failure(
                InfrastructureDetail::makeWin32Error(
                    "open the project directory for canonical identity", nativeError,
                    Domain::ErrorCodes::PathOutsideAuthority));
        }

        auto inspected = inspectDirectoryHandle(
            handle.get(), lexical.value(), "inspect the canonical project directory");
        if (!inspected) {
            return Domain::Result<CanonicalProjectDirectory>::failure(
                std::move(inspected).error());
        }
        auto finalPath = finalPathForHandle(
            handle.get(), "resolve the canonical project directory");
        if (!finalPath) {
            return Domain::Result<CanonicalProjectDirectory>::failure(
                std::move(finalPath).error());
        }
        auto pathText = InfrastructureDetail::WindowsPathResolver::toPathText(finalPath.value());
        if (!pathText) {
            return Domain::Result<CanonicalProjectDirectory>::failure(
                std::move(pathText).error());
        }
        return Domain::Result<CanonicalProjectDirectory>::success(CanonicalProjectDirectory{
            std::move(pathText).value(), std::move(finalPath).value(), std::move(handle)});
    } catch (...) {
        return Domain::Result<CanonicalProjectDirectory>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "project_path could not be resolved through a retained Windows handle."));
    }
}

[[nodiscard]] bool missingPathError(const DWORD nativeError) noexcept
{
    return nativeError == ERROR_FILE_NOT_FOUND || nativeError == ERROR_PATH_NOT_FOUND ||
           nativeError == ERROR_DIRECTORY || nativeError == ERROR_NOT_FOUND;
}

[[nodiscard]] Domain::Result<void> inspectGitFileHandle(const HANDLE handle) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(InfrastructureDetail::makeWin32Error(
            "inspect the project Git configuration", ::GetLastError(),
            Domain::ErrorCodes::PathOutsideAuthority));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The project Git configuration is a reparse point or is not a regular file."));
    }
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<void>::failure(InfrastructureDetail::makeWin32Error(
            "inspect the project Git configuration", ::GetLastError(),
            Domain::ErrorCodes::PathOutsideAuthority));
    }
    if (standard.DeletePending != FALSE || standard.NumberOfLinks != 1U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The project Git configuration is pending deletion or has multiple hard links."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::vector<std::byte>> readBoundedFile(
    const HANDLE handle,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = InfrastructureDetail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "read the project Git configuration");
        if (!validContext) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(validContext).error());
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(handle, &size) == FALSE) {
            return Domain::Result<std::vector<std::byte>>::failure(
                InfrastructureDetail::makeWin32Error(
                    "measure the project Git configuration", ::GetLastError(),
                    Domain::ErrorCodes::PathOutsideAuthority));
        }
        if (size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) > maximumBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project Git configuration exceeds its 1 MiB limit."));
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset{};
        while (offset < bytes.size()) {
            validContext = InfrastructureDetail::validateOperationContext(
                context, std::chrono::steady_clock::now(),
                "read the project Git configuration");
            if (!validContext) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(validContext).error());
            }
            const DWORD requested = static_cast<DWORD>((std::min)(
                bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read{};
            if (::ReadFile(handle, bytes.data() + offset, requested, &read, nullptr) == FALSE) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    InfrastructureDetail::makeWin32Error(
                        "read the project Git configuration", ::GetLastError(),
                        Domain::ErrorCodes::PathOutsideAuthority));
            }
            if (read == 0U) {
                return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The project Git configuration ended before its retained size."));
            }
            offset += static_cast<std::size_t>(read);
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project Git configuration could not be read."));
    }
}

[[nodiscard]] Domain::Result<std::optional<std::string>> inferredRepositoryIdentity(
    const CanonicalProjectDirectory& project,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context) noexcept
{
    try {
        InfrastructureDetail::RelativeOpenOptions directoryOptions{};
        directoryOptions.desiredAccess =
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
        directoryOptions.shareAccess = FILE_SHARE_READ;
        directoryOptions.objectType = InfrastructureDetail::RelativeObjectType::Directory;
        const auto gitDirectory = InfrastructureDetail::openRelative(
            project.handle.get(), L".git", directoryOptions);
        if (!gitDirectory) {
            if (missingPathError(gitDirectory.win32Error) ||
                gitDirectory.win32Error == ERROR_NOT_A_REPARSE_POINT) {
                return Domain::Result<std::optional<std::string>>::success(std::nullopt);
            }
            return Domain::Result<std::optional<std::string>>::failure(
                InfrastructureDetail::makeWin32Error(
                    "open the project-owned .git directory", gitDirectory.win32Error,
                    Domain::ErrorCodes::PathOutsideAuthority));
        }

        std::wstring gitPath = project.nativePath;
        gitPath.append(L"\\.git");
        auto inspectedDirectory = inspectDirectoryHandle(
            gitDirectory.handle.get(), gitPath, "inspect the project-owned .git directory");
        if (!inspectedDirectory) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(inspectedDirectory).error());
        }

        InfrastructureDetail::RelativeOpenOptions fileOptions{};
        fileOptions.desiredAccess = FILE_READ_DATA | FILE_READ_ATTRIBUTES;
        fileOptions.shareAccess = FILE_SHARE_READ;
        fileOptions.objectType = InfrastructureDetail::RelativeObjectType::File;
        fileOptions.sequentialAccess = true;
        const auto config = InfrastructureDetail::openRelative(
            gitDirectory.handle.get(), L"config", fileOptions);
        if (!config) {
            if (missingPathError(config.win32Error)) {
                return Domain::Result<std::optional<std::string>>::success(std::nullopt);
            }
            return Domain::Result<std::optional<std::string>>::failure(
                InfrastructureDetail::makeWin32Error(
                    "open the project-owned Git configuration", config.win32Error,
                    Domain::ErrorCodes::PathOutsideAuthority));
        }
        auto inspectedFile = inspectGitFileHandle(config.handle.get());
        if (!inspectedFile) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(inspectedFile).error());
        }
        auto content = readBoundedFile(
            config.handle.get(), WindowsProjectRegistryRepository::MaximumGitConfigBytes,
            context);
        if (!content) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(content).error());
        }
        const std::string text{
            reinterpret_cast<const char*>(content.value().data()), content.value().size()};
        if (text.find('\0') != std::string::npos) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The project Git configuration contains a NUL byte."));
        }
        auto validUtf8 = InfrastructureDetail::strictUtf8ToUtf16(text);
        if (!validUtf8) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The project Git configuration is not valid UTF-8."));
        }

        std::vector<std::string> remotes;
        std::size_t offset{};
        while (offset <= text.size()) {
            const auto newline = text.find('\n', offset);
            const auto length = newline == std::string::npos
                                    ? text.size() - offset
                                    : newline - offset;
            auto line = trimAscii(text.substr(offset, length));
            const auto equals = line.find('=');
            if (equals != std::string::npos &&
                lowerAscii(trimAscii(line.substr(0U, equals))) == "url") {
                auto remote = trimAscii(line.substr(equals + 1U));
                if (!remote.empty()) {
                    if (remote.size() >
                        WindowsProjectRegistryRepository::MaximumRepositoryIdentityBytes) {
                        return Domain::Result<std::optional<std::string>>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::PayloadTooLarge,
                                "A project Git remote URL exceeds its byte limit."));
                    }
                    remotes.push_back("url = " + std::move(remote));
                    if (remotes.size() >
                        WindowsProjectRegistryRepository::MaximumGitRemoteCount) {
                        return Domain::Result<std::optional<std::string>>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::LimitExceeded,
                                "The project Git configuration contains too many remote URLs."));
                    }
                }
            }
            if (newline == std::string::npos) {
                break;
            }
            offset = newline + 1U;
        }
        if (remotes.empty()) {
            return Domain::Result<std::optional<std::string>>::success(std::nullopt);
        }
        std::sort(remotes.begin(), remotes.end());
        remotes.erase(std::unique(remotes.begin(), remotes.end()), remotes.end());

        std::string joined;
        for (std::size_t index = 0U; index < remotes.size(); ++index) {
            if (index != 0U) {
                joined.push_back('\n');
            }
            joined.append(remotes[index]);
        }
        const auto characters = std::span<const char>{joined.data(), joined.size()};
        auto digest = hasher.sha256(std::as_bytes(characters));
        if (!digest) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(digest).error());
        }
        return Domain::Result<std::optional<std::string>>::success(
            std::optional<std::string>{"git:" + digest.value().value()});
    } catch (...) {
        return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The optional project Git identity could not be derived."));
    }
}

void requireObjectKeys(
    const Json& value,
    const std::span<const std::string_view> expected,
    const std::string_view description)
{
    if (!value.is_object() || value.size() != expected.size()) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{description} + " has missing, extra, or invalid fields.");
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (std::find(expected.begin(), expected.end(), key) == expected.end()) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{description} + " contains an unknown field.");
        }
    }
}

[[nodiscard]] std::string requiredString(
    const Json& object,
    const std::string_view key,
    const std::size_t maximumBytes,
    const std::string_view description)
{
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{description} + " must be a string.");
    }
    auto value = found->get<std::string>();
    if (value.empty() || value.size() > maximumBytes || containsControlCharacter(value)) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{description} + " is empty, contains controls, or exceeds its limit.");
    }
    if (!InfrastructureDetail::strictUtf8ToUtf16(value)) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{description} + " is not valid UTF-8.");
    }
    return value;
}

[[nodiscard]] RegistryEntry decodeEntry(const Json& value)
{
    constexpr std::array<std::string_view, 6U> EntryKeys{
        "id", "displayName", "repositoryIdentity", "aliases", "createdAt", "updatedAt"};
    requireObjectKeys(value, EntryKeys, "A project-registry entry");

    auto idText = requiredString(value, "id", 36U, "Project id");
    auto id = Domain::ProjectId::parse(idText);
    if (!id || id.value().value() != idText) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-registry id is not a lowercase canonical UUID.");
    }
    auto displayName = requiredString(
        value, "displayName", MaximumDisplayNameBytes, "Project display name");
    if (trimAscii(displayName) != displayName) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "A project display name is not normalized.");
    }

    std::optional<std::string> repositoryIdentity;
    const auto identity = value.find("repositoryIdentity");
    if (identity == value.end() || (!identity->is_null() && !identity->is_string())) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "Project repositoryIdentity must be a string or null.");
    }
    if (identity->is_string()) {
        auto raw = identity->get<std::string>();
        auto normalized = normalizeRepositoryIdentity(std::optional<std::string>{raw});
        if (!normalized || !normalized.value() || normalized.value().value() != raw) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "A project repositoryIdentity is invalid or not normalized.");
        }
        repositoryIdentity = std::move(normalized).value();
    }

    const auto aliasesValue = value.find("aliases");
    if (aliasesValue == value.end() || !aliasesValue->is_array() ||
        aliasesValue->size() > WindowsProjectRegistryRepository::MaximumAliasCount) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "Project aliases must be an array within the 32-alias limit.");
    }
    std::vector<Domain::PathText> aliases;
    aliases.reserve(aliasesValue->size());
    for (const auto& aliasValue : *aliasesValue) {
        if (!aliasValue.is_string()) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "Every project alias must be a string.");
        }
        auto path = Domain::PathText::create(aliasValue.get_ref<const std::string&>());
        if (!path) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "A project alias exceeds its path-text limit.");
        }
        auto normalized = normalizeLocalAliasLexically(path.value());
        if (!normalized || normalized.value().value() != path.value().value()) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "A project alias is not a canonical local-drive path.");
        }
        aliases.push_back(std::move(normalized).value());
    }
    std::sort(aliases.begin(), aliases.end(), aliasLess);
    if (std::adjacent_find(aliases.begin(), aliases.end(), sameAlias) != aliases.end()) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-registry entry contains a duplicate alias.");
    }

    auto createdAt = requiredString(
        value, "createdAt", MaximumTimestampBytes, "Project createdAt");
    auto updatedAt = requiredString(
        value, "updatedAt", MaximumTimestampBytes, "Project updatedAt");
    if (!validTimestamp(createdAt) || !validTimestamp(updatedAt)) {
        rejectDocument(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-registry timestamp is not canonical UTC text.");
    }
    return RegistryEntry{
        std::move(id).value(), std::move(displayName), std::move(repositoryIdentity),
        std::move(aliases), std::move(createdAt), std::move(updatedAt)};
}

[[nodiscard]] Domain::Result<RegistryDocument> parseDocument(
    const std::span<const std::byte> bytes) noexcept
{
    try {
        if (bytes.size() > WindowsProjectRegistryRepository::MaximumDocumentBytes) {
            return Domain::Result<RegistryDocument>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project registry exceeds its 2 MiB limit."));
        }
        const std::string text{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        std::vector<std::unordered_set<std::string>> objectKeys;
        const auto callback = [&objectKeys](
                                  const int depth,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (depth < 0 || static_cast<std::size_t>(depth) >
                                 WindowsProjectRegistryRepository::MaximumJsonDepth) {
                rejectDocument(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Project-registry JSON nesting exceeds depth 32.");
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() ||
                    !objectKeys.back().insert(parsed.get<std::string>()).second) {
                    rejectDocument(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Project-registry JSON contains a duplicate object key.");
                }
            } else if (event == Json::parse_event_t::object_end) {
                if (objectKeys.empty()) {
                    rejectDocument(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Project-registry object nesting is malformed.");
                }
                objectKeys.pop_back();
            }
            return true;
        };

        const auto root = Json::parse(text, callback, true, false);
        constexpr std::array<std::string_view, 2U> RootKeys{
            "schemaVersion", "projects"};
        requireObjectKeys(root, RootKeys, "The project registry");

        const auto schema = root.find("schemaVersion");
        if (schema == root.end() || !schema->is_number_unsigned()) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "Project-registry schemaVersion must be an unsigned integer.");
        }
        const auto schemaVersion = schema->get<std::uint64_t>();
        if (schemaVersion != WindowsProjectRegistryRepository::SchemaVersion) {
            rejectDocument(
                Domain::ErrorCodes::UnsupportedVersion,
                "The project-registry schema version is not supported.");
        }

        const auto projects = root.find("projects");
        if (projects == root.end() || !projects->is_array() ||
            projects->size() > WindowsProjectRegistryRepository::MaximumProjectCount) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "Project-registry projects must be an array within the 1024-project limit.");
        }
        RegistryDocument document;
        document.projects.reserve(projects->size());
        for (const auto& value : *projects) {
            document.projects.push_back(decodeEntry(value));
        }
        sortDocument(document);

        std::unordered_set<std::string> ids;
        std::unordered_set<std::string> repositoryIdentities;
        std::vector<Domain::PathText> allAliases;
        for (const auto& entry : document.projects) {
            if (!ids.insert(entry.id.value()).second) {
                rejectDocument(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The project registry contains a duplicate project id.");
            }
            if (entry.repositoryIdentity &&
                !repositoryIdentities.insert(*entry.repositoryIdentity).second) {
                rejectDocument(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The project registry contains a duplicate repository identity.");
            }
            allAliases.insert(
                allAliases.end(), entry.aliases.begin(), entry.aliases.end());
        }
        std::sort(allAliases.begin(), allAliases.end(), aliasLess);
        if (std::adjacent_find(allAliases.begin(), allAliases.end(), sameAlias) !=
            allAliases.end()) {
            rejectDocument(
                Domain::ErrorCodes::IntegrityFailure,
                "The project registry assigns one alias to several projects.");
        }
        return Domain::Result<RegistryDocument>::success(std::move(document));
    } catch (const RegistryDocumentException& error) {
        return Domain::Result<RegistryDocument>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (const Json::parse_error&) {
        return Domain::Result<RegistryDocument>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The project registry is not valid strict JSON."));
    } catch (...) {
        return Domain::Result<RegistryDocument>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project registry could not be parsed."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::byte>> serializeDocument(
    RegistryDocument document) noexcept
{
    try {
        sortDocument(document);
        Json root = Json::object();
        root["schemaVersion"] = WindowsProjectRegistryRepository::SchemaVersion;
        auto& projects = root["projects"] = Json::array();
        for (const auto& entry : document.projects) {
            Json encoded = Json::object();
            encoded["id"] = entry.id.value();
            encoded["displayName"] = entry.displayName;
            if (entry.repositoryIdentity) {
                encoded["repositoryIdentity"] = *entry.repositoryIdentity;
            } else {
                encoded["repositoryIdentity"] = nullptr;
            }
            auto& aliases = encoded["aliases"] = Json::array();
            for (const auto& alias : entry.aliases) {
                aliases.push_back(alias.value());
            }
            encoded["createdAt"] = entry.createdAt;
            encoded["updatedAt"] = entry.updatedAt;
            projects.push_back(std::move(encoded));
        }
        auto text = root.dump(2, ' ', false, Json::error_handler_t::strict);
        text.push_back('\n');
        if (text.size() > WindowsProjectRegistryRepository::MaximumDocumentBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The updated project registry would exceed its 2 MiB limit."));
        }
        std::vector<std::byte> bytes(text.size());
        if (!text.empty()) {
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project registry could not be serialized."));
    }
}

struct ResolvedStoragePaths final {
    std::wstring dataRoot;
    std::wstring primary;
    std::wstring backup;
    std::wstring lock;
};

[[nodiscard]] std::wstring childPath(
    const std::wstring_view root,
    const std::wstring_view relative)
{
    std::wstring value{root};
    if (!value.ends_with(L'\\')) {
        value.push_back(L'\\');
    }
    value.append(relative);
    return value;
}

[[nodiscard]] bool capabilityPathEquals(
    const Contracts::AuthorizedPath& capability,
    const std::wstring_view expectedPath,
    const std::wstring_view expectedRoot,
    const Domain::FileAccess expectedAccess) noexcept
{
    auto path = InfrastructureDetail::strictUtf8ToUtf16(
        capability.canonicalPath().value());
    auto root = InfrastructureDetail::strictUtf8ToUtf16(
        capability.authorityRoot().value());
    return capability.access() == expectedAccess && path && root &&
           ordinalEqualIgnoreCase(path.value(), expectedPath) &&
           ordinalEqualIgnoreCase(root.value(), expectedRoot);
}

[[nodiscard]] Domain::Result<ResolvedStoragePaths> resolveStoragePaths(
    Contracts::IApplicationPaths& applicationPaths,
    const WindowsProjectRegistryStoragePaths& capabilities,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto dataRootText = applicationPaths.dataRoot(context);
        if (!dataRootText) {
            return Domain::Result<ResolvedStoragePaths>::failure(
                std::move(dataRootText).error());
        }
        auto dataRoot = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(
            dataRootText.value().value());
        if (!dataRoot) {
            return Domain::Result<ResolvedStoragePaths>::failure(
                std::move(dataRoot).error());
        }
        ResolvedStoragePaths paths{
            dataRoot.value(),
            childPath(dataRoot.value(), WindowsProjectRegistryRepository::RegistryRelativePath),
            childPath(
                dataRoot.value(),
                WindowsProjectRegistryRepository::RegistryBackupRelativePath),
            childPath(dataRoot.value(),
                      WindowsProjectRegistryRepository::RegistryLockRelativePath)};

        const bool oneAuthority =
            capabilities.readPath.authorityId() == capabilities.writePath.authorityId() &&
            capabilities.readPath.authorityId() == capabilities.createPath.authorityId() &&
            capabilities.readPath.authorityId() == capabilities.backupReadPath.authorityId();
        if (!oneAuthority ||
            !capabilityPathEquals(
                capabilities.readPath, paths.primary, paths.dataRoot,
                Domain::FileAccess::Read) ||
            !capabilityPathEquals(
                capabilities.writePath, paths.primary, paths.dataRoot,
                Domain::FileAccess::Write) ||
            !capabilityPathEquals(
                capabilities.createPath, paths.primary, paths.dataRoot,
                Domain::FileAccess::Create) ||
            !capabilityPathEquals(
                capabilities.backupReadPath, paths.backup, paths.dataRoot,
                Domain::FileAccess::Read)) {
            return Domain::Result<ResolvedStoragePaths>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Project-registry storage capabilities do not bind the fixed app-data registry and sibling backup."));
        }
        return Domain::Result<ResolvedStoragePaths>::success(std::move(paths));
    } catch (...) {
        return Domain::Result<ResolvedStoragePaths>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The fixed project-registry storage paths could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<UniqueHandle> openDriveAnchor(
    const std::wstring_view driveRoot) noexcept
{
    const auto native = extendedPath(driveRoot);
    UniqueHandle handle{::CreateFileW(
        native.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle) {
        return Domain::Result<UniqueHandle>::failure(
            InfrastructureDetail::makeWin32Error(
                "anchor the project-registry drive", ::GetLastError(),
                Domain::ErrorCodes::PathOutsideAuthority));
    }
    auto inspected = inspectDirectoryHandle(
        handle.get(), driveRoot, "inspect the project-registry drive");
    if (!inspected) {
        return Domain::Result<UniqueHandle>::failure(std::move(inspected).error());
    }
    return Domain::Result<UniqueHandle>::success(std::move(handle));
}

[[nodiscard]] Domain::Result<UniqueHandle> openOrCreateDirectoryRelative(
    const HANDLE parent,
    const std::wstring_view component,
    const std::wstring_view expectedPath) noexcept
{
    InfrastructureDetail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
    options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    options.objectType = InfrastructureDetail::RelativeObjectType::Directory;
    auto opened = InfrastructureDetail::openRelative(parent, component, options);
    if (!opened && missingPathError(opened.win32Error)) {
        options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenOrCreate;
        options.fileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        opened = InfrastructureDetail::openRelative(parent, component, options);
    }
    if (!opened) {
        return Domain::Result<UniqueHandle>::failure(
            InfrastructureDetail::makeWin32Error(
                "open or create a project-registry directory", opened.win32Error,
                Domain::ErrorCodes::PathOutsideAuthority));
    }
    auto inspected = inspectDirectoryHandle(
        opened.handle.get(), expectedPath,
        "inspect an anchored project-registry directory");
    if (!inspected) {
        return Domain::Result<UniqueHandle>::failure(std::move(inspected).error());
    }
    return Domain::Result<UniqueHandle>::success(std::move(opened.handle));
}

struct RegistryDirectoryAnchors final {
    std::vector<UniqueHandle> handles;
    HANDLE projectsDirectory{};
};

[[nodiscard]] Domain::Result<RegistryDirectoryAnchors> openRegistryDirectories(
    const std::wstring_view dataRoot) noexcept
{
    try {
        if (dataRoot.size() <= 3U || dataRoot[1] != L':' || dataRoot[2] != L'\\') {
            return Domain::Result<RegistryDirectoryAnchors>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The project-registry data root is not a bounded local-drive path."));
        }
        std::wstring fullPath{dataRoot};
        fullPath.append(L"\\projects");
        std::wstring current{fullPath.substr(0U, 3U)};
        auto drive = openDriveAnchor(current);
        if (!drive) {
            return Domain::Result<RegistryDirectoryAnchors>::failure(
                std::move(drive).error());
        }
        RegistryDirectoryAnchors result;
        result.handles.push_back(std::move(drive).value());

        std::size_t componentStart = 3U;
        while (componentStart < fullPath.size()) {
            const auto separator = fullPath.find(L'\\', componentStart);
            const auto componentEnd = separator == std::wstring::npos
                                          ? fullPath.size()
                                          : separator;
            const auto component = fullPath.substr(
                componentStart, componentEnd - componentStart);
            if (!current.ends_with(L'\\')) {
                current.push_back(L'\\');
            }
            current.append(component);
            auto opened = openOrCreateDirectoryRelative(
                result.handles.back().get(), component, current);
            if (!opened) {
                return Domain::Result<RegistryDirectoryAnchors>::failure(
                    std::move(opened).error());
            }
            result.handles.push_back(std::move(opened).value());
            if (separator == std::wstring::npos) {
                break;
            }
            componentStart = separator + 1U;
        }
        result.projectsDirectory = result.handles.back().get();
        return Domain::Result<RegistryDirectoryAnchors>::success(std::move(result));
    } catch (...) {
        return Domain::Result<RegistryDirectoryAnchors>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-registry directory anchors could not be acquired."));
    }
}

[[nodiscard]] Domain::Result<void> inspectRegistryLockHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(InfrastructureDetail::makeWin32Error(
            "inspect the project-registry lock", ::GetLastError(),
            Domain::ErrorCodes::PathOutsideAuthority));
    }
    if ((attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The project-registry lock is a reparse point or directory."));
    }
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<void>::failure(InfrastructureDetail::makeWin32Error(
            "inspect the project-registry lock", ::GetLastError(),
            Domain::ErrorCodes::PathOutsideAuthority));
    }
    if (standard.DeletePending != FALSE || standard.NumberOfLinks != 1U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The project-registry lock is pending deletion or has multiple hard links."));
    }
    auto finalPath = finalPathForHandle(handle, "resolve the project-registry lock");
    if (!finalPath) {
        return Domain::Result<void>::failure(std::move(finalPath).error());
    }
    if (!ordinalEqualIgnoreCase(finalPath.value(), expectedPath) &&
        !InfrastructureDetail::WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, finalPath.value())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The opened project-registry lock differs from its fixed app-data path."));
    }
    return Domain::Result<void>::success();
}

class RegistryLock final {
public:
    RegistryLock(std::vector<UniqueHandle> anchors, UniqueHandle lockHandle) noexcept
        : anchors_{std::move(anchors)}, lockHandle_{std::move(lockHandle)}
    {
    }

    RegistryLock(const RegistryLock&) = delete;
    RegistryLock& operator=(const RegistryLock&) = delete;
    RegistryLock(RegistryLock&&) noexcept = default;
    RegistryLock& operator=(RegistryLock&&) = delete;

    ~RegistryLock() noexcept
    {
        if (lockHandle_) {
            OVERLAPPED operation{};
            static_cast<void>(::UnlockFileEx(
                lockHandle_.get(), 0U, 1U, 0U, &operation));
        }
    }

private:
    std::vector<UniqueHandle> anchors_;
    UniqueHandle lockHandle_;
};

[[nodiscard]] DWORD waitMillisecondsUntil(
    const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const auto count = remaining.count() + 1;
    return static_cast<DWORD>((std::min<long long>)(count, MAXDWORD - 1LL));
}

[[nodiscard]] Domain::Result<RegistryLock> acquireRegistryLock(
    const ResolvedStoragePaths& paths,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = InfrastructureDetail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "acquire the project-registry lock");
        if (!validContext) {
            return Domain::Result<RegistryLock>::failure(
                std::move(validContext).error());
        }
        auto directories = openRegistryDirectories(paths.dataRoot);
        if (!directories) {
            return Domain::Result<RegistryLock>::failure(
                std::move(directories).error());
        }

        const auto started = std::chrono::steady_clock::now();
        const auto busyDeadline = started + RegistryLockMaximumWait;
        const bool contextDeadlineWins = context.deadline <= busyDeadline;
        const auto effectiveDeadline = contextDeadlineWins
                                           ? context.deadline
                                           : busyDeadline;
        UniqueHandle cancellationEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!cancellationEvent) {
            return Domain::Result<RegistryLock>::failure(
                InfrastructureDetail::makeWin32Error(
                    "create the project-registry cancellation event", ::GetLastError()));
        }
        std::stop_callback cancellationWake{
            context.cancellation,
            [event = cancellationEvent.get()]() noexcept {
                static_cast<void>(::SetEvent(event));
            }};

        const auto admissionFailure = [&]() -> Domain::Result<RegistryLock> {
            if (context.isCancellationRequested()) {
                return Domain::Result<RegistryLock>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The project-registry lock wait was cancelled."));
            }
            if (contextDeadlineWins) {
                return Domain::Result<RegistryLock>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The project-registry lock exceeded the operation deadline."));
            }
            return Domain::Result<RegistryLock>::failure(Domain::makeError(
                Domain::ErrorCodes::DatabaseBusy,
                "The project registry remained locked for three seconds.", true));
        };

        for (;;) {
            if (context.isCancellationRequested() ||
                std::chrono::steady_clock::now() >= effectiveDeadline) {
                return admissionFailure();
            }
            InfrastructureDetail::RelativeOpenOptions options{};
            options.desiredAccess = GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES;
            options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
            options.disposition =
                InfrastructureDetail::RelativeOpenDisposition::OpenOrCreate;
            options.fileAttributes =
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            options.objectType = InfrastructureDetail::RelativeObjectType::File;
            options.writeThrough = true;
            auto opened = InfrastructureDetail::openRelative(
                directories.value().projectsDirectory, L".registry.lock", options);
            if (opened) {
                auto inspected = inspectRegistryLockHandle(
                    opened.handle.get(), paths.lock);
                if (!inspected) {
                    return Domain::Result<RegistryLock>::failure(
                        std::move(inspected).error());
                }
                OVERLAPPED operation{};
                if (::LockFileEx(
                        opened.handle.get(),
                        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0U, 1U, 0U, &operation) != FALSE) {
                    if (context.isCancellationRequested() ||
                        std::chrono::steady_clock::now() >= effectiveDeadline) {
                        static_cast<void>(::UnlockFileEx(
                            opened.handle.get(), 0U, 1U, 0U, &operation));
                        return admissionFailure();
                    }
                    return Domain::Result<RegistryLock>::success(RegistryLock{
                        std::move(directories).value().handles,
                        std::move(opened.handle)});
                }
                const DWORD lockError = ::GetLastError();
                if (lockError != ERROR_LOCK_VIOLATION &&
                    lockError != ERROR_SHARING_VIOLATION) {
                    return Domain::Result<RegistryLock>::failure(
                        InfrastructureDetail::makeWin32Error(
                            "acquire the project-registry byte lock", lockError));
                }
            } else if (opened.win32Error != ERROR_SHARING_VIOLATION &&
                       opened.win32Error != ERROR_LOCK_VIOLATION) {
                return Domain::Result<RegistryLock>::failure(
                    InfrastructureDetail::makeWin32Error(
                        "open the project-registry lock", opened.win32Error));
            }

            const auto retryDeadline = (std::min)(
                effectiveDeadline,
                std::chrono::steady_clock::now() + RegistryLockRetrySlice);
            const DWORD waited = ::WaitForSingleObject(
                cancellationEvent.get(), waitMillisecondsUntil(retryDeadline));
            if (waited == WAIT_OBJECT_0) {
                return admissionFailure();
            }
            if (waited != WAIT_TIMEOUT) {
                return Domain::Result<RegistryLock>::failure(
                    InfrastructureDetail::makeWin32Error(
                        "wait to retry the project-registry lock",
                        waited == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_FUNCTION));
            }
        }
    } catch (...) {
        return Domain::Result<RegistryLock>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-registry lock could not be acquired."));
    }
}

struct LoadedRegistry final {
    RegistryDocument document;
    bool primaryExists{};
};

[[nodiscard]] bool missingRegistryRead(const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::RecordNotFound;
}

[[nodiscard]] bool recoverableRegistryRead(const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::RecordNotFound ||
           error.code == Domain::ErrorCodes::PayloadTooLarge ||
           error.code == Domain::ErrorCodes::IntegrityFailure ||
           error.code == Domain::ErrorCodes::MalformedMessage;
}

[[nodiscard]] Domain::Result<LoadedRegistry> loadRegistry(
    Contracts::IAtomicFileStore& fileStore,
    const WindowsProjectRegistryStoragePaths& paths,
    const Domain::OperationContext& context) noexcept
{
    auto primary = fileStore.read(
        paths.readPath,
        WindowsProjectRegistryRepository::MaximumDocumentBytes,
        context);
    if (primary) {
        auto parsed = parseDocument(primary.value());
        if (parsed) {
            return Domain::Result<LoadedRegistry>::success(
                LoadedRegistry{std::move(parsed).value(), true});
        }
        if (parsed.error().code == Domain::ErrorCodes::UnsupportedVersion ||
            !recoverableRegistryRead(parsed.error())) {
            return Domain::Result<LoadedRegistry>::failure(std::move(parsed).error());
        }
    } else if (!recoverableRegistryRead(primary.error())) {
        return Domain::Result<LoadedRegistry>::failure(std::move(primary).error());
    }

    const bool primaryExists = primary || !missingRegistryRead(primary.error());
    auto backup = fileStore.read(
        paths.backupReadPath,
        WindowsProjectRegistryRepository::MaximumDocumentBytes,
        context);
    if (backup) {
        auto parsed = parseDocument(backup.value());
        if (parsed) {
            return Domain::Result<LoadedRegistry>::success(
                LoadedRegistry{std::move(parsed).value(), primaryExists});
        }
        if (parsed.error().code == Domain::ErrorCodes::UnsupportedVersion) {
            return Domain::Result<LoadedRegistry>::failure(std::move(parsed).error());
        }
    } else if (!recoverableRegistryRead(backup.error())) {
        return Domain::Result<LoadedRegistry>::failure(std::move(backup).error());
    }

    if (!primaryExists && !backup && missingRegistryRead(backup.error())) {
        return Domain::Result<LoadedRegistry>::success(
            LoadedRegistry{RegistryDocument{}, false});
    }
    return Domain::Result<LoadedRegistry>::failure(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The project registry is invalid and no valid sibling backup is available."));
}

[[nodiscard]] Domain::Result<void> persistRegistry(
    Contracts::IAtomicFileStore& fileStore,
    const WindowsProjectRegistryStoragePaths& paths,
    RegistryDocument document,
    const bool primaryExists,
    const Domain::OperationContext& context) noexcept
{
    auto serialized = serializeDocument(std::move(document));
    if (!serialized) {
        return Domain::Result<void>::failure(std::move(serialized).error());
    }
    auto validContext = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(), "publish the project registry");
    if (!validContext) {
        return validContext;
    }
    const auto& destination = primaryExists ? paths.writePath : paths.createPath;
    return fileStore.replace(destination, serialized.value(), true, context);
}

[[nodiscard]] std::optional<std::size_t> indexForProjectId(
    const RegistryDocument& document,
    const Domain::ProjectId& id) noexcept
{
    const auto found = std::find_if(
        document.projects.begin(), document.projects.end(),
        [&](const RegistryEntry& entry) { return entry.id == id; });
    return found == document.projects.end()
               ? std::nullopt
               : std::optional<std::size_t>{static_cast<std::size_t>(
                     std::distance(document.projects.begin(), found))};
}

[[nodiscard]] std::optional<std::size_t> indexForRepositoryIdentity(
    const RegistryDocument& document,
    const std::optional<std::string>& identity) noexcept
{
    if (!identity) {
        return std::nullopt;
    }
    const auto found = std::find_if(
        document.projects.begin(), document.projects.end(),
        [&](const RegistryEntry& entry) {
            return entry.repositoryIdentity && *entry.repositoryIdentity == *identity;
        });
    return found == document.projects.end()
               ? std::nullopt
               : std::optional<std::size_t>{static_cast<std::size_t>(
                     std::distance(document.projects.begin(), found))};
}

[[nodiscard]] std::optional<std::size_t> indexForAlias(
    const RegistryDocument& document,
    const Domain::PathText& alias) noexcept
{
    const auto found = std::find_if(
        document.projects.begin(), document.projects.end(),
        [&](const RegistryEntry& entry) {
            return std::find_if(
                       entry.aliases.begin(), entry.aliases.end(),
                       [&](const Domain::PathText& existing) {
                           return sameAlias(existing, alias);
                       }) != entry.aliases.end();
        });
    return found == document.projects.end()
               ? std::nullopt
               : std::optional<std::size_t>{static_cast<std::size_t>(
                     std::distance(document.projects.begin(), found))};
}

[[nodiscard]] bool evidenceDisagrees(
    const std::optional<std::size_t> left,
    const std::optional<std::size_t> right) noexcept
{
    return left && right && *left != *right;
}

[[nodiscard]] Domain::Result<std::string> normalizedDisplayName(
    const std::optional<std::string>& supplied,
    const Domain::PathText& canonicalPath,
    const Domain::ProjectMemoryLimits& limits) noexcept
{
    const auto maximumBytes = (std::min)(limits.maximumTitleBytes, MaximumDisplayNameBytes);
    auto normalized = normalizeOptionalText(supplied, maximumBytes, "display_name");
    if (!normalized) {
        return Domain::Result<std::string>::failure(std::move(normalized).error());
    }
    if (normalized.value()) {
        return Domain::Result<std::string>::success(
            std::move(normalized).value().value());
    }
    const auto separator = canonicalPath.value().find_last_of("\\/");
    auto inferred = separator == std::string::npos
                        ? canonicalPath.value()
                        : canonicalPath.value().substr(separator + 1U);
    if (inferred.empty() || inferred.size() > maximumBytes) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The inferred project display name exceeds its UTF-8 byte limit."));
    }
    return Domain::Result<std::string>::success(std::move(inferred));
}

[[nodiscard]] Domain::ProjectInitialization initializationFor(
    const RegistryEntry& entry,
    const Domain::ProjectMemoryLimits& limits)
{
    return Domain::ProjectInitialization{
        descriptorFor(entry),
        Domain::ProjectMemorySchemaVersion,
        Domain::ProjectMemoryCapabilityVersion,
        limits,
        true,
        false,
        true};
}

} // namespace

class WindowsProjectRegistryRepository::Impl final {
public:
    Impl(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
        WindowsProjectRegistryStoragePaths storagePaths,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IClock> clock,
        Domain::ProjectMemoryLimits limits)
        : applicationPaths_{std::move(applicationPaths)},
          atomicFileStore_{std::move(atomicFileStore)},
          storagePaths_{std::move(storagePaths)},
          uuidGenerator_{std::move(uuidGenerator)},
          hasher_{std::move(hasher)},
          clock_{std::move(clock)},
          limits_{limits}
    {
        if (!applicationPaths_ || !atomicFileStore_ || !uuidGenerator_ || !hasher_ ||
            !clock_ || limits_.maximumTitleBytes == 0U) {
            throw std::invalid_argument{
                "Project-registry dependencies and limits must be valid."};
        }
    }

    ~Impl() noexcept { executor_.shutdown(); }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Project-registry initialize");
        if (!lease) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(lease).error());
        }

        auto canonical = canonicalProjectDirectory(request.projectPath);
        if (!canonical) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(canonical).error());
        }
        auto suppliedIdentity = normalizeRepositoryIdentity(request.repositoryIdentity);
        if (!suppliedIdentity) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(suppliedIdentity).error());
        }
        auto inferredIdentity = inferredRepositoryIdentity(
            canonical.value(), *hasher_, context);
        if (!inferredIdentity) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(inferredIdentity).error());
        }
        if (suppliedIdentity.value() && inferredIdentity.value() &&
            suppliedIdentity.value().value() != inferredIdentity.value().value()) {
            return Domain::Result<Domain::ProjectInitialization>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The supplied repository identity conflicts with the canonical project's Git identity."));
        }
        const auto repositoryIdentity = suppliedIdentity.value()
                                            ? suppliedIdentity.value()
                                            : inferredIdentity.value();
        auto displayName = normalizedDisplayName(
            request.displayName, canonical.value().path, limits_);
        if (!displayName) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(displayName).error());
        }

        auto paths = resolveStoragePaths(*applicationPaths_, storagePaths_, context);
        if (!paths) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(paths).error());
        }
        auto registryLock = acquireRegistryLock(paths.value(), context);
        if (!registryLock) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(registryLock).error());
        }
        auto loaded = loadRegistry(*atomicFileStore_, storagePaths_, context);
        if (!loaded) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(loaded).error());
        }
        auto timestamp = formatUtcTimestamp(*clock_);
        if (!timestamp) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(timestamp).error());
        }

        auto loadedRegistry = std::move(loaded).value();
        const bool primaryExists = loadedRegistry.primaryExists;
        auto document = std::move(loadedRegistry.document);
        std::optional<std::size_t> requestedIndex;
        if (request.requestedProjectId) {
            requestedIndex = indexForProjectId(document, *request.requestedProjectId);
            if (!requestedIndex) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The explicitly requested project id is not registered."));
            }
        }
        const auto identityIndex = indexForRepositoryIdentity(document, repositoryIdentity);
        const auto aliasIndex = indexForAlias(document, canonical.value().path);
        if (evidenceDisagrees(requestedIndex, identityIndex) ||
            evidenceDisagrees(requestedIndex, aliasIndex) ||
            evidenceDisagrees(identityIndex, aliasIndex)) {
            return Domain::Result<Domain::ProjectInitialization>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "Requested id, repository identity, and canonical alias identify different projects."));
        }

        const auto selected = requestedIndex
                                  ? requestedIndex
                                  : (identityIndex ? identityIndex : aliasIndex);
        std::size_t selectedIndex{};
        if (selected) {
            selectedIndex = *selected;
            auto& entry = document.projects[selectedIndex];
            if (repositoryIdentity && entry.repositoryIdentity &&
                *repositoryIdentity != *entry.repositoryIdentity) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectScopeMismatch,
                        "The canonical project conflicts with the registered repository identity."));
            }
            if (!aliasIndex) {
                if (entry.aliases.size() >= MaximumAliasCount) {
                    return Domain::Result<Domain::ProjectInitialization>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The project already contains the maximum 32 aliases."));
                }
                entry.aliases.push_back(canonical.value().path);
                std::sort(entry.aliases.begin(), entry.aliases.end(), aliasLess);
            }
            if (!entry.repositoryIdentity && repositoryIdentity) {
                entry.repositoryIdentity = repositoryIdentity;
            }
            if (request.displayName) {
                entry.displayName = displayName.value();
            }
            entry.updatedAt = timestamp.value();
        } else {
            if (document.projects.size() >= MaximumProjectCount) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The project registry already contains 1024 projects."));
            }
            auto generated = uuidGenerator_->next();
            if (!generated) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    std::move(generated).error());
            }
            Domain::ProjectId projectId{std::move(generated).value()};
            if (indexForProjectId(document, projectId)) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The UUID generator produced an existing project id."));
            }
            document.projects.push_back(RegistryEntry{
                std::move(projectId), std::move(displayName).value(), repositoryIdentity,
                std::vector<Domain::PathText>{canonical.value().path}, timestamp.value(),
                timestamp.value()});
            sortDocument(document);
            const auto inserted = indexForAlias(document, canonical.value().path);
            if (!inserted) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "The inserted project could not be located deterministically."));
            }
            selectedIndex = *inserted;
        }

        auto persisted = persistRegistry(
            *atomicFileStore_, storagePaths_, document, primaryExists, context);
        if (!persisted) {
            return Domain::Result<Domain::ProjectInitialization>::failure(
                std::move(persisted).error());
        }
        return Domain::Result<Domain::ProjectInitialization>::success(
            initializationFor(document.projects[selectedIndex], limits_));
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Project-registry descriptor");
        if (!lease) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(lease).error());
        }
        auto paths = resolveStoragePaths(*applicationPaths_, storagePaths_, context);
        if (!paths) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(paths).error());
        }
        auto registryLock = acquireRegistryLock(paths.value(), context);
        if (!registryLock) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(registryLock).error());
        }
        auto loaded = loadRegistry(*atomicFileStore_, storagePaths_, context);
        if (!loaded) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(loaded).error());
        }
        const auto index = indexForProjectId(loaded.value().document, projectId);
        if (!index) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ProjectNotFound,
                    "The requested project is not registered."));
        }
        return Domain::Result<Domain::ProjectMemoryDescriptor>::success(
            descriptorFor(loaded.value().document.projects[*index]));
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Project-registry list");
        if (!lease) {
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                std::move(lease).error());
        }
        auto paths = resolveStoragePaths(*applicationPaths_, storagePaths_, context);
        if (!paths) {
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                std::move(paths).error());
        }
        auto registryLock = acquireRegistryLock(paths.value(), context);
        if (!registryLock) {
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                std::move(registryLock).error());
        }
        auto loaded = loadRegistry(*atomicFileStore_, storagePaths_, context);
        if (!loaded) {
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                std::move(loaded).error());
        }
        const auto count = (std::min)({
            maximumCount,
            loaded.value().document.projects.size(),
            MaximumProjectCount});
        std::vector<Domain::ProjectMemoryDescriptor> descriptors;
        descriptors.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            descriptors.push_back(descriptorFor(
                loaded.value().document.projects[index]));
        }
        return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::success(
            std::move(descriptors));
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId& projectId,
        const Domain::PathText& alias,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Project-registry detach alias");
        if (!lease) {
            return Domain::Result<void>::failure(std::move(lease).error());
        }
        auto normalizedAlias = normalizeLocalAliasLexically(alias);
        if (!normalizedAlias) {
            return Domain::Result<void>::failure(std::move(normalizedAlias).error());
        }
        auto paths = resolveStoragePaths(*applicationPaths_, storagePaths_, context);
        if (!paths) {
            return Domain::Result<void>::failure(std::move(paths).error());
        }
        auto registryLock = acquireRegistryLock(paths.value(), context);
        if (!registryLock) {
            return Domain::Result<void>::failure(std::move(registryLock).error());
        }
        auto loaded = loadRegistry(*atomicFileStore_, storagePaths_, context);
        if (!loaded) {
            return Domain::Result<void>::failure(std::move(loaded).error());
        }
        auto loadedRegistry = std::move(loaded).value();
        const bool primaryExists = loadedRegistry.primaryExists;
        auto document = std::move(loadedRegistry.document);
        const auto projectIndex = indexForProjectId(document, projectId);
        if (!projectIndex) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectNotFound,
                "The requested project is not registered."));
        }
        const auto aliasIndex = indexForAlias(document, normalizedAlias.value());
        if (aliasIndex && *aliasIndex != *projectIndex) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The alias belongs to a different registered project."));
        }
        if (!aliasIndex) {
            return Domain::Result<void>::success();
        }

        auto& entry = document.projects[*projectIndex];
        entry.aliases.erase(
            std::remove_if(
                entry.aliases.begin(), entry.aliases.end(),
                [&](const Domain::PathText& existing) {
                    return sameAlias(existing, normalizedAlias.value());
                }),
            entry.aliases.end());
        auto timestamp = formatUtcTimestamp(*clock_);
        if (!timestamp) {
            return Domain::Result<void>::failure(std::move(timestamp).error());
        }
        entry.updatedAt = std::move(timestamp).value();
        return persistRegistry(
            *atomicFileStore_, storagePaths_, std::move(document), primaryExists, context);
    }

private:
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths_;
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore_;
    WindowsProjectRegistryStoragePaths storagePaths_;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    std::shared_ptr<Contracts::IHasher> hasher_;
    std::shared_ptr<Contracts::IClock> clock_;
    Domain::ProjectMemoryLimits limits_;
    InfrastructureDetail::BoundedSerialExecutor executor_;
};

WindowsProjectRegistryRepository::WindowsProjectRegistryRepository(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
    WindowsProjectRegistryStoragePaths storagePaths,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IClock> clock,
    Domain::ProjectMemoryLimits limits)
    : implementation_{std::make_unique<Impl>(
          std::move(applicationPaths), std::move(atomicFileStore),
          std::move(storagePaths), std::move(uuidGenerator), std::move(hasher),
          std::move(clock), limits)}
{
}

WindowsProjectRegistryRepository::~WindowsProjectRegistryRepository() noexcept = default;

Domain::Result<Domain::ProjectInitialization>
WindowsProjectRegistryRepository::initialize(
    const Domain::InitializeProjectRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->initialize(request, context);
    } catch (...) {
        return Domain::Result<Domain::ProjectInitialization>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-registry initialization failed at the Windows boundary."));
    }
}

Domain::Result<Domain::ProjectMemoryDescriptor>
WindowsProjectRegistryRepository::descriptor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->descriptor(projectId, context);
    } catch (...) {
        return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Project-registry descriptor lookup failed at the Windows boundary."));
    }
}

Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>
WindowsProjectRegistryRepository::list(
    const std::size_t maximumCount,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->list(maximumCount, context);
    } catch (...) {
        return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Project-registry enumeration failed at the Windows boundary."));
    }
}

Domain::Result<void> WindowsProjectRegistryRepository::detachAlias(
    const Domain::ProjectId& projectId,
    const Domain::PathText& alias,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->detachAlias(projectId, alias, context);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-registry alias detachment failed at the Windows boundary."));
    }
}

} // namespace ForgeConductor::Persistence::Windows
