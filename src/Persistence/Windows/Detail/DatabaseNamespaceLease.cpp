#include "DatabaseNamespaceLease.h"

#include "../../../Infrastructure/Windows/Detail/RelativeFileOperations.h"
#include "../../../Infrastructure/Windows/Detail/Win32Error.h"
#include "../../../Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;

constexpr ACCESS_MASK DirectoryAnchorAccess =
    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
constexpr ULONG DirectoryShare = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr std::size_t MaximumNativePathCharacters = 32'760U;
constexpr auto MaximumDirectoryComponentOpenWait = std::chrono::milliseconds{500};
constexpr DWORD DirectoryComponentOpenRetrySliceMilliseconds = 10U;
constexpr auto MaximumMigrationLockWait = std::chrono::seconds{3};
constexpr auto MigrationLockRetrySlice = std::chrono::milliseconds{10};
constexpr ULONG NativeFileRenameInformationEx = 65U;
constexpr std::array<DatabaseLeafRole, 3U> IdentityTrackedCohortRoles{
    DatabaseLeafRole::Main,
    DatabaseLeafRole::Wal,
    DatabaseLeafRole::SharedMemory};
constexpr std::array<DatabaseLeafRole, 4U> QuarantineCohortRoles{
    DatabaseLeafRole::Main,
    DatabaseLeafRole::Wal,
    DatabaseLeafRole::SharedMemory,
    DatabaseLeafRole::Journal};

[[nodiscard]] constexpr std::size_t roleIndex(const DatabaseLeafRole role) noexcept
{
    switch (role) {
    case DatabaseLeafRole::Main:
        return 0U;
    case DatabaseLeafRole::Wal:
        return 1U;
    case DatabaseLeafRole::SharedMemory:
        return 2U;
    case DatabaseLeafRole::Journal:
        return 3U;
    case DatabaseLeafRole::MigrationLock:
        return 4U;
    }
    return 0U;
}

[[nodiscard]] constexpr bool validRole(const DatabaseLeafRole role) noexcept
{
    return role == DatabaseLeafRole::Main || role == DatabaseLeafRole::Wal ||
           role == DatabaseLeafRole::SharedMemory || role == DatabaseLeafRole::Journal ||
           role == DatabaseLeafRole::MigrationLock;
}

[[nodiscard]] bool equalInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size() || left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return ::CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] int compareInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return 0;
    }
    const int comparison = ::CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE);
    if (comparison == CSTR_LESS_THAN) {
        return -1;
    }
    if (comparison == CSTR_GREATER_THAN) {
        return 1;
    }
    return 0;
}

[[nodiscard]] Domain::Error namespaceError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A database namespace operation failed and its diagnostic could not be retained.",
            retryable);
    }
}

[[nodiscard]] Domain::Error fileError(
    const std::string_view action,
    const DWORD nativeCode,
    const std::string_view code = Domain::ErrorCodes::InternalFailure,
    const bool retryable = false) noexcept
{
    return InfrastructureDetail::makeWin32Error(action, nativeCode, code, retryable);
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view path)
{
    std::wstring result{L"\\\\?\\"};
    result.append(path);
    return result;
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value)
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

[[nodiscard]] bool validLeafName(const std::wstring_view value) noexcept
{
    if (value.empty() || value.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters ||
        value == L"." || value == L".." || value.back() == L' ' || value.back() == L'.') {
        return false;
    }
    const bool invalidCharacter = std::any_of(
        value.begin(), value.end(), [](const wchar_t character) noexcept {
            return character < 0x20 || character == L'\\' || character == L'/' ||
                   character == L':' || character == L'<' || character == L'>' ||
                   character == L'"' || character == L'|' || character == L'?' ||
                   character == L'*';
        });
    if (invalidCharacter) {
        return false;
    }

    std::wstring_view base = value.substr(0U, value.find(L'.'));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) {
        base.remove_suffix(1U);
    }
    const auto equals = [base](const std::wstring_view reserved) noexcept {
        return base.size() == reserved.size() &&
               ::CompareStringOrdinal(
                   base.data(), static_cast<int>(base.size()),
                   reserved.data(), static_cast<int>(reserved.size()), TRUE) == CSTR_EQUAL;
    };
    if (equals(L"CON") || equals(L"PRN") || equals(L"AUX") || equals(L"NUL") ||
        equals(L"CONIN$") || equals(L"CONOUT$") || equals(L"CLOCK$")) {
        return false;
    }
    if (base.size() != 4U) {
        return true;
    }
    const bool reservedDigit = (base[3] >= L'1' && base[3] <= L'9') ||
                               base[3] == L'\u00b9' || base[3] == L'\u00b2' ||
                               base[3] == L'\u00b3';
    return !reservedDigit ||
           !(::CompareStringOrdinal(base.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
             ::CompareStringOrdinal(base.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL);
}

[[nodiscard]] bool validLeafFragment(const std::wstring_view value) noexcept
{
    if (value.empty() || value.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters) {
        return false;
    }
    return std::none_of(
        value.begin(), value.end(), [](const wchar_t character) noexcept {
            return character < 0x20 || character == L'\\' || character == L'/' ||
                   character == L':' || character == L'<' || character == L'>' ||
                   character == L'"' || character == L'|' || character == L'?' ||
                   character == L'*';
        });
}

[[nodiscard]] Domain::Result<std::wstring> validateCanonicalDirectory(
    const std::wstring_view candidate) noexcept
{
    try {
        if (candidate.size() <= 3U || candidate.size() > MaximumNativePathCharacters) {
            return Domain::Result<std::wstring>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database directory must be a bounded canonical absolute path on a local drive."));
        }
        const bool driveLetter = (candidate[0] >= L'A' && candidate[0] <= L'Z') ||
                                 (candidate[0] >= L'a' && candidate[0] <= L'z');
        if (!driveLetter || candidate[1] != L':' || candidate[2] != L'\\' ||
            candidate.back() == L'\\' || candidate.find(L'/') != std::wstring_view::npos ||
            candidate.starts_with(L"\\\\") || candidate.starts_with(L"\\\\?\\") ||
            candidate.starts_with(L"\\??\\")) {
            return Domain::Result<std::wstring>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database directory must be a canonical absolute path on a local drive."));
        }

        std::size_t componentStart = 3U;
        while (componentStart < candidate.size()) {
            const std::size_t separator = candidate.find(L'\\', componentStart);
            const std::size_t componentEnd =
                separator == std::wstring_view::npos ? candidate.size() : separator;
            if (!validLeafName(candidate.substr(componentStart, componentEnd - componentStart))) {
                return Domain::Result<std::wstring>::failure(namespaceError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A database directory contains an unsafe Windows path component."));
            }
            componentStart = componentEnd + 1U;
        }

        const std::wstring input{candidate};
        const DWORD required = ::GetFullPathNameW(input.c_str(), 0U, nullptr, nullptr);
        if (required == 0U || required > MaximumNativePathCharacters + 1U) {
            return Domain::Result<std::wstring>::failure(
                fileError("canonicalize a database directory", ::GetLastError()));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFullPathNameW(
            input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<std::wstring>::failure(
                fileError("canonicalize a database directory", ::GetLastError()));
        }
        std::wstring canonical{buffer.data(), static_cast<std::size_t>(written)};
        if (!equalInsensitive(candidate, canonical)) {
            return Domain::Result<std::wstring>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "The database directory changes under Windows lexical normalization."));
        }

        std::wstring driveRoot{canonical.substr(0U, 3U)};
        if (::GetDriveTypeW(driveRoot.c_str()) != DRIVE_FIXED) {
            return Domain::Result<std::wstring>::failure(namespaceError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Database storage must reside on a fixed local Windows volume."));
        }
        return Domain::Result<std::wstring>::success(std::move(canonical));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database directory could not be validated."));
    }
}

[[nodiscard]] Domain::Result<std::string> strictUtf16ToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database path cannot be represented as bounded UTF-8."));
        }
        const int inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return Domain::Result<std::string>::failure(
                fileError("convert a database path to UTF-8", ::GetLastError()));
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required, nullptr, nullptr) != required) {
            return Domain::Result<std::string>::failure(
                fileError("convert a database path to UTF-8", ::GetLastError()));
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A database path could not be converted to UTF-8."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> openedPath(const HANDLE handle) noexcept
{
    try {
        const DWORD required = ::GetFinalPathNameByHandleW(
            handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U || required > MaximumNativePathCharacters + 4U) {
            return Domain::Result<std::wstring>::failure(
                fileError("resolve an anchored database path", ::GetLastError()));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<std::wstring>::failure(
                fileError("resolve an anchored database path", ::GetLastError()));
        }
        return Domain::Result<std::wstring>::success(withoutExtendedPrefix(
            std::wstring{buffer.data(), static_cast<std::size_t>(written)}));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "An anchored database path could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<DatabaseFileIdentity> fileIdentity(
    const HANDLE handle,
    const std::string_view action) noexcept
{
    FILE_ID_INFO information{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &information, sizeof(information)) == FALSE) {
        return Domain::Result<DatabaseFileIdentity>::failure(
            fileError(action, ::GetLastError()));
    }
    DatabaseFileIdentity result{};
    result.volumeSerialNumber = information.VolumeSerialNumber;
    std::memcpy(
        result.fileIdentifier.data(), information.FileId.Identifier,
        result.fileIdentifier.size());
    return Domain::Result<DatabaseFileIdentity>::success(result);
}

[[nodiscard]] Domain::Result<void> verifyDirectoryHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(
            fileError("inspect an anchored database directory", ::GetLastError()));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "A database directory anchor is not a regular non-reparse directory."));
    }

    FILE_CASE_SENSITIVE_INFO caseSensitivity{};
    if (::GetFileInformationByHandleEx(
            handle, FileCaseSensitiveInfo, &caseSensitivity, sizeof(caseSensitivity)) == FALSE) {
        return Domain::Result<void>::failure(
            fileError("inspect database directory case-sensitivity", ::GetLastError()));
    }
    if ((caseSensitivity.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0U) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "Case-sensitive directories are not permitted in a database namespace."));
    }

    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<void>::failure(
            fileError("inspect database directory state", ::GetLastError()));
    }
    if (standard.Directory == FALSE || standard.DeletePending != FALSE) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::IntegrityFailure,
            "A database directory anchor is no longer a live directory."));
    }

    auto resolved = openedPath(handle);
    if (!resolved) {
        return Domain::Result<void>::failure(std::move(resolved).error());
    }
    if (!equalInsensitive(expectedPath, resolved.value()) &&
        !InfrastructureDetail::WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, resolved.value())) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "An opened database directory differs from its canonical path."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<DatabaseFileIdentity> verifyFileHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<DatabaseFileIdentity>::failure(
            fileError("inspect an anchored database leaf", ::GetLastError()));
    }
    if ((attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return Domain::Result<DatabaseFileIdentity>::failure(namespaceError(
            Domain::ErrorCodes::IntegrityFailure,
            "A database leaf must be a regular non-reparse file."));
    }

    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<DatabaseFileIdentity>::failure(
            fileError("inspect database leaf state", ::GetLastError()));
    }
    if (standard.Directory != FALSE || standard.DeletePending != FALSE ||
        standard.NumberOfLinks != 1U) {
        return Domain::Result<DatabaseFileIdentity>::failure(namespaceError(
            Domain::ErrorCodes::IntegrityFailure,
            "A database leaf must be live, regular, and single-link."));
    }

    auto resolved = openedPath(handle);
    if (!resolved) {
        return Domain::Result<DatabaseFileIdentity>::failure(std::move(resolved).error());
    }
    if (!equalInsensitive(expectedPath, resolved.value()) &&
        !InfrastructureDetail::WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, resolved.value())) {
        return Domain::Result<DatabaseFileIdentity>::failure(namespaceError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "An opened database leaf differs from its canonical path."));
    }
    return fileIdentity(handle, "read an anchored database leaf identity");
}

struct AnchoredDirectoryTree final {
    std::vector<std::wstring> paths;
    std::vector<InfrastructureDetail::UniqueHandle> handles;
};

[[nodiscard]] constexpr bool transientDirectoryComponentOpenError(
    const DWORD nativeCode) noexcept
{
    return nativeCode == ERROR_SHARING_VIOLATION ||
           nativeCode == ERROR_LOCK_VIOLATION;
}

[[nodiscard]] InfrastructureDetail::RelativeOpenResult openDirectoryComponent(
    const HANDLE anchoredParent,
    const std::wstring_view directoryName,
    const InfrastructureDetail::RelativeOpenOptions& options,
    const std::chrono::steady_clock::time_point contentionDeadline) noexcept
{
    auto opened = InfrastructureDetail::openRelative(
        anchoredParent, directoryName, options);
    while (!opened && transientDirectoryComponentOpenError(opened.win32Error) &&
           std::chrono::steady_clock::now() < contentionDeadline) {
        ::Sleep(DirectoryComponentOpenRetrySliceMilliseconds);
        opened = InfrastructureDetail::openRelative(
            anchoredParent, directoryName, options);
    }
    return opened;
}

[[nodiscard]] Domain::Result<InfrastructureDetail::UniqueHandle> openRootAnchor(
    const std::wstring_view rootPath) noexcept
{
    try {
        const std::wstring nativePath = extendedPath(rootPath);
        InfrastructureDetail::UniqueHandle handle{::CreateFileW(
            nativePath.c_str(), DirectoryAnchorAccess, DirectoryShare,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle) {
            return Domain::Result<InfrastructureDetail::UniqueHandle>::failure(
                fileError("open the database volume-root anchor", ::GetLastError()));
        }
        auto verified = verifyDirectoryHandle(handle.get(), rootPath);
        if (!verified) {
            return Domain::Result<InfrastructureDetail::UniqueHandle>::failure(
                std::move(verified).error());
        }
        return Domain::Result<InfrastructureDetail::UniqueHandle>::success(std::move(handle));
    } catch (...) {
        return Domain::Result<InfrastructureDetail::UniqueHandle>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database volume-root anchor could not be opened."));
    }
}

[[nodiscard]] Domain::Result<InfrastructureDetail::UniqueHandle> reopenForDirectoryCreation(
    const HANDLE anchoredParent,
    const std::wstring_view directoryName,
    const std::wstring_view expectedPath,
    const std::chrono::steady_clock::time_point contentionDeadline) noexcept
{
    InfrastructureDetail::RelativeOpenOptions options{};
    options.desiredAccess = DirectoryAnchorAccess | FILE_ADD_SUBDIRECTORY;
    options.shareAccess = DirectoryShare;
    options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
    options.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    options.objectType = InfrastructureDetail::RelativeObjectType::Directory;
    auto reopened = openDirectoryComponent(
        anchoredParent, directoryName, options, contentionDeadline);
    if (!reopened) {
        const bool transientContention =
            transientDirectoryComponentOpenError(reopened.win32Error);
        return Domain::Result<InfrastructureDetail::UniqueHandle>::failure(
            fileError(
                "reopen an anchored database directory for child creation",
                reopened.win32Error,
                transientContention ? Domain::ErrorCodes::DatabaseBusy
                                    : Domain::ErrorCodes::InternalFailure,
                transientContention));
    }
    auto verified = verifyDirectoryHandle(reopened.handle.get(), expectedPath);
    if (!verified) {
        return Domain::Result<InfrastructureDetail::UniqueHandle>::failure(
            std::move(verified).error());
    }
    return Domain::Result<InfrastructureDetail::UniqueHandle>::success(
        std::move(reopened.handle));
}

[[nodiscard]] Domain::Result<AnchoredDirectoryTree> createAndAnchorDirectoryTree(
    const std::wstring_view canonicalDirectory) noexcept
{
    try {
        AnchoredDirectoryTree result;
        const auto contentionDeadline =
            std::chrono::steady_clock::now() + MaximumDirectoryComponentOpenWait;
        result.paths.emplace_back(canonicalDirectory.substr(0U, 3U));
        auto root = openRootAnchor(result.paths.back());
        if (!root) {
            return Domain::Result<AnchoredDirectoryTree>::failure(std::move(root).error());
        }
        result.handles.push_back(std::move(root).value());

        std::size_t componentStart = 3U;
        while (componentStart < canonicalDirectory.size()) {
            const std::size_t separator = canonicalDirectory.find(L'\\', componentStart);
            const std::size_t componentEnd =
                separator == std::wstring_view::npos ? canonicalDirectory.size() : separator;
            const std::wstring_view component =
                canonicalDirectory.substr(componentStart, componentEnd - componentStart);
            const bool finalComponent = componentEnd == canonicalDirectory.size();
            const std::wstring expectedPath{canonicalDirectory.substr(0U, componentEnd)};

            InfrastructureDetail::RelativeOpenOptions options{};
            options.desiredAccess = DirectoryAnchorAccess |
                                    (finalComponent ? FILE_ADD_FILE : 0U);
            options.shareAccess = DirectoryShare;
            options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
            options.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            options.objectType = InfrastructureDetail::RelativeObjectType::Directory;
            auto opened = openDirectoryComponent(
                result.handles.back().get(), component, options, contentionDeadline);
            if (!opened &&
                (opened.win32Error == ERROR_FILE_NOT_FOUND || opened.win32Error == ERROR_PATH_NOT_FOUND)) {
                if (result.handles.size() < 2U) {
                    return Domain::Result<AnchoredDirectoryTree>::failure(namespaceError(
                        Domain::ErrorCodes::Unauthorized,
                        "A database directory cannot be created directly below the volume root."));
                }
                const std::wstring& creationParentPath = result.paths.back();
                const std::size_t parentSeparator = creationParentPath.find_last_of(L'\\');
                if (parentSeparator == std::wstring::npos ||
                    parentSeparator + 1U >= creationParentPath.size()) {
                    return Domain::Result<AnchoredDirectoryTree>::failure(namespaceError(
                        Domain::ErrorCodes::InternalFailure,
                        "The anchored creation parent has no valid leaf name."));
                }
                const std::wstring_view creationParentName{
                    creationParentPath.data() + parentSeparator + 1U,
                    creationParentPath.size() - parentSeparator - 1U};
                auto creationParent = reopenForDirectoryCreation(
                    result.handles[result.handles.size() - 2U].get(),
                    creationParentName,
                    creationParentPath,
                    contentionDeadline);
                if (!creationParent) {
                    return Domain::Result<AnchoredDirectoryTree>::failure(
                        std::move(creationParent).error());
                }
                options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenOrCreate;
                opened = openDirectoryComponent(
                    creationParent.value().get(), component, options,
                    contentionDeadline);
            }
            if (!opened) {
                const bool transientContention =
                    transientDirectoryComponentOpenError(opened.win32Error);
                return Domain::Result<AnchoredDirectoryTree>::failure(fileError(
                    "open or create an anchored database directory component",
                    opened.win32Error,
                    transientContention ? Domain::ErrorCodes::DatabaseBusy
                                        : Domain::ErrorCodes::InternalFailure,
                    transientContention));
            }
            auto verified = verifyDirectoryHandle(opened.handle.get(), expectedPath);
            if (!verified) {
                return Domain::Result<AnchoredDirectoryTree>::failure(
                    std::move(verified).error());
            }
            result.paths.push_back(expectedPath);
            result.handles.push_back(std::move(opened.handle));
            componentStart = componentEnd + 1U;
        }
        return Domain::Result<AnchoredDirectoryTree>::success(std::move(result));
    } catch (...) {
        return Domain::Result<AnchoredDirectoryTree>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database directory tree could not be securely created and anchored."));
    }
}

[[nodiscard]] InfrastructureDetail::RelativeOpenDisposition nativeDisposition(
    const DatabaseLeafDisposition disposition) noexcept
{
    switch (disposition) {
    case DatabaseLeafDisposition::OpenExisting:
        return InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
    case DatabaseLeafDisposition::CreateNew:
        return InfrastructureDetail::RelativeOpenDisposition::CreateNew;
    case DatabaseLeafDisposition::OpenOrCreate:
        return InfrastructureDetail::RelativeOpenDisposition::OpenOrCreate;
    }
    return InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
}

[[nodiscard]] bool missingFileError(const DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] Domain::Error operationInterruption(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    if (context.isCancellationRequested()) {
        return namespaceError(
            Domain::ErrorCodes::Cancelled,
            std::string{action} + " was cancelled.");
    }
    return namespaceError(
        Domain::ErrorCodes::DeadlineExceeded,
        std::string{action} + " exceeded its deadline.");
}

[[nodiscard]] Domain::Result<void> checkOperation(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    if (context.isCancellationRequested() ||
        context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(operationInterruption(context, action));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] DWORD waitMillisecondsUntil(
    const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    constexpr auto MaximumWait = static_cast<long long>((std::numeric_limits<DWORD>::max)() - 1U);
    return static_cast<DWORD>((std::min)(remaining, MaximumWait));
}

struct NativeIoStatusBlock final {
    union {
        LONG status;
        void* pointer;
    } result{};
    ULONG_PTR information{};
};

using NtSetInformationFileFunction =
    LONG(NTAPI*)(HANDLE, NativeIoStatusBlock*, void*, ULONG, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(LONG);

[[nodiscard]] DWORD renameRelativeToSourceDirectory(
    const HANDLE source,
    FILE_RENAME_INFO* const information,
    const ULONG informationBytes) noexcept
{
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return ERROR_PROC_NOT_FOUND;
    }
    const auto ntSetInformationFile = reinterpret_cast<NtSetInformationFileFunction>(
        ::GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto rtlNtStatusToDosError = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr) {
        return ERROR_PROC_NOT_FOUND;
    }

    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSetInformationFile(
        source,
        &ioStatus,
        information,
        informationBytes,
        NativeFileRenameInformationEx);
    return status >= 0
               ? ERROR_SUCCESS
               : static_cast<DWORD>(rtlNtStatusToDosError(status));
}

void unlockByteZero(const HANDLE handle) noexcept
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    OVERLAPPED operation{};
    if (::UnlockFileEx(handle, 0U, 1U, 0U, &operation) == FALSE &&
        ::GetLastError() == ERROR_IO_PENDING) {
        DWORD ignored{};
        static_cast<void>(::GetOverlappedResult(handle, &operation, &ignored, TRUE));
    }
}

} // namespace

struct DatabaseNamespaceState final {
    std::wstring canonicalDirectory;
    std::array<std::wstring, 5U> leafNames;
    std::array<std::wstring, 5U> canonicalPaths;
    std::array<std::string, 5U> canonicalUtf8Paths;
    std::vector<std::wstring> anchorPaths;
    std::vector<InfrastructureDetail::UniqueHandle> anchors;
    DatabaseFileIdentity directoryIdentity{};
    std::mutex cohortMutex;
    std::array<std::optional<DatabaseFileIdentity>, 3U> expectedCohortIdentities;
    std::array<std::size_t, 3U> retainedCohortOwners{};
    std::mutex enumerationMutex;
    std::atomic_size_t openVfsFiles{};
};

namespace {

[[nodiscard]] constexpr std::optional<std::size_t> cohortIdentityIndex(
    const DatabaseLeafRole role) noexcept
{
    switch (role) {
    case DatabaseLeafRole::Main:
        return 0U;
    case DatabaseLeafRole::Wal:
        return 1U;
    case DatabaseLeafRole::SharedMemory:
        return 2U;
    case DatabaseLeafRole::Journal:
    case DatabaseLeafRole::MigrationLock:
        return std::nullopt;
    }
    return std::nullopt;
}

struct CohortIdentityObservation final {
    InfrastructureDetail::UniqueHandle handle;
    DatabaseFileIdentity identity{};
};

[[nodiscard]] Domain::Result<bool> isUnlinkedTransientLeaf(
    const HANDLE handle) noexcept
{
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        return Domain::Result<bool>::failure(
            fileError("inspect a transient database leaf state", ::GetLastError()));
    }
    return Domain::Result<bool>::success(
        standard.DeletePending != FALSE || standard.NumberOfLinks == 0U);
}

[[nodiscard]] Domain::Result<std::optional<CohortIdentityObservation>> inspectCohortLeaf(
    const DatabaseNamespaceState& state,
    const DatabaseLeafRole role) noexcept
{
    InfrastructureDetail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_READ_ATTRIBUTES;
    // Retain every observation until the cohort comparison completes so a
    // pathname cannot be renamed or replaced between verification and use.
    options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
    options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    options.objectType = InfrastructureDetail::RelativeObjectType::File;
    const std::size_t index = roleIndex(role);
    auto opened = InfrastructureDetail::openRelative(
        state.anchors.back().get(), state.leafNames[index], options);
    constexpr std::size_t MaximumTransientSidecarOpenAttempts = 8U;
    for (std::size_t attempt = 1U;
         !opened && role != DatabaseLeafRole::Main &&
         (opened.win32Error == ERROR_ACCESS_DENIED ||
          opened.win32Error == ERROR_SHARING_VIOLATION) &&
         attempt < MaximumTransientSidecarOpenAttempts;
         ++attempt) {
        ::Sleep(1U);
        opened = InfrastructureDetail::openRelative(
            state.anchors.back().get(), state.leafNames[index], options);
    }
    if (!opened) {
        if (missingFileError(opened.win32Error)) {
            return Domain::Result<std::optional<CohortIdentityObservation>>::success(
                std::nullopt);
        }
        return Domain::Result<std::optional<CohortIdentityObservation>>::failure(fileError(
            "open a database cohort leaf for identity revalidation",
            opened.win32Error,
            opened.win32Error == ERROR_SHARING_VIOLATION
                ? Domain::ErrorCodes::DatabaseBusy
                : Domain::ErrorCodes::InternalFailure,
            opened.win32Error == ERROR_SHARING_VIOLATION));
    }
    if (role != DatabaseLeafRole::Main) {
        auto unlinked = isUnlinkedTransientLeaf(opened.handle.get());
        if (!unlinked) {
            return Domain::Result<std::optional<CohortIdentityObservation>>::failure(
                std::move(unlinked).error());
        }
        if (unlinked.value()) {
            return Domain::Result<std::optional<CohortIdentityObservation>>::success(
                std::nullopt);
        }
    }
    auto identity = verifyFileHandle(opened.handle.get(), state.canonicalPaths[index]);
    if (!identity) {
        if (role != DatabaseLeafRole::Main) {
            auto unlinked = isUnlinkedTransientLeaf(opened.handle.get());
            if (unlinked && unlinked.value()) {
                return Domain::Result<std::optional<CohortIdentityObservation>>::success(
                    std::nullopt);
            }
        }
        return Domain::Result<std::optional<CohortIdentityObservation>>::failure(
            std::move(identity).error());
    }
    return Domain::Result<std::optional<CohortIdentityObservation>>::success(
        CohortIdentityObservation{std::move(opened.handle), identity.value()});
}

[[nodiscard]] Domain::Result<void> rememberCohortIdentity(
    DatabaseNamespaceState& state,
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& identity) noexcept
{
    const auto index = cohortIdentityIndex(role);
    if (!index.has_value()) {
        return Domain::Result<void>::success();
    }
    std::scoped_lock lock{state.cohortMutex};
    auto& expected = state.expectedCohortIdentities[*index];
    if (expected.has_value() && *expected != identity) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Conflict,
            "A database cohort leaf changed from its remembered identity.",
            true));
    }
    if (role == DatabaseLeafRole::Main || expected.has_value()) {
        expected = identity;
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> retainCohortIdentity(
    DatabaseNamespaceState& state,
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& identity) noexcept
{
    const auto index = cohortIdentityIndex(role);
    if (!index.has_value()) {
        return Domain::Result<void>::success();
    }
    std::scoped_lock lock{state.cohortMutex};
    auto& expected = state.expectedCohortIdentities[*index];
    if (expected.has_value() && *expected != identity) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Conflict,
            "A database cohort leaf changed from its retained identity.",
            true));
    }
    auto& owners = state.retainedCohortOwners[*index];
    if (owners == (std::numeric_limits<std::size_t>::max)()) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::LimitExceeded,
            "Database cohort ownership exceeded its bounded counter."));
    }
    expected = identity;
    ++owners;
    return Domain::Result<void>::success();
}

void releaseCohortIdentity(
    DatabaseNamespaceState& state,
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& identity) noexcept
{
    const auto index = cohortIdentityIndex(role);
    if (!index.has_value()) {
        return;
    }
    std::scoped_lock lock{state.cohortMutex};
    auto& owners = state.retainedCohortOwners[*index];
    if (owners != 0U) {
        --owners;
    }
    static_cast<void>(role);
    static_cast<void>(identity);
}

void clearCohortIdentity(
    DatabaseNamespaceState& state,
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& deletedIdentity) noexcept
{
    const auto index = cohortIdentityIndex(role);
    if (!index.has_value()) {
        return;
    }
    std::scoped_lock lock{state.cohortMutex};
    auto& expected = state.expectedCohortIdentities[*index];
    if (state.retainedCohortOwners[*index] == 0U &&
        (!expected.has_value() || *expected == deletedIdentity)) {
        expected.reset();
    }
}

void transferCohortIdentity(
    DatabaseNamespaceState& source,
    const DatabaseLeafRole sourceRole,
    DatabaseNamespaceState& destination,
    const DatabaseLeafRole destinationRole,
    const DatabaseFileIdentity& identity) noexcept
{
    const auto sourceIndex = cohortIdentityIndex(sourceRole);
    const auto destinationIndex = cohortIdentityIndex(destinationRole);
    if (!sourceIndex.has_value() && !destinationIndex.has_value()) {
        return;
    }
    if (&source == &destination) {
        std::scoped_lock lock{source.cohortMutex};
        if (sourceIndex.has_value()) {
            source.expectedCohortIdentities[*sourceIndex].reset();
        }
        if (destinationIndex.has_value()) {
            destination.expectedCohortIdentities[*destinationIndex] = identity;
        }
        return;
    }
    std::scoped_lock lock{source.cohortMutex, destination.cohortMutex};
    if (sourceIndex.has_value()) {
        source.expectedCohortIdentities[*sourceIndex].reset();
    }
    if (destinationIndex.has_value()) {
        destination.expectedCohortIdentities[*destinationIndex] = identity;
    }
}

[[nodiscard]] Domain::Result<void> revalidateNamespaceAnchors(
    const DatabaseNamespaceState& state) noexcept
{
    if (state.anchors.empty() || state.anchors.size() != state.anchorPaths.size()) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::IntegrityFailure,
            "The database namespace has incomplete directory anchors."));
    }
    for (std::size_t index = 0U; index < state.anchors.size(); ++index) {
        auto verified = verifyDirectoryHandle(
            state.anchors[index].get(), state.anchorPaths[index]);
        if (!verified) {
            return verified;
        }
    }
    auto identity = fileIdentity(
        state.anchors.back().get(), "revalidate the database directory identity");
    if (!identity) {
        return Domain::Result<void>::failure(std::move(identity).error());
    }
    if (identity.value() != state.directoryIdentity) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Conflict,
            "The database directory identity changed after it was anchored.",
            true));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool matchesLeafFragments(
    const std::wstring_view name,
    const std::wstring_view prefix,
    const std::wstring_view suffix) noexcept
{
    return name.size() >= prefix.size() + suffix.size() &&
           equalInsensitive(name.substr(0U, prefix.size()), prefix) &&
           equalInsensitive(name.substr(name.size() - suffix.size()), suffix);
}

} // namespace

DatabaseLeafLease::DatabaseLeafLease(
    const DatabaseLeafRole role,
    InfrastructureDetail::UniqueHandle handle,
    const DatabaseFileIdentity identity,
    const bool wasCreated,
    std::shared_ptr<DatabaseNamespaceState> state) noexcept
    : role_{role}, handle_{std::move(handle)}, identity_{identity}, wasCreated_{wasCreated},
      state_{std::move(state)}
{
}

DatabaseLeafLease::~DatabaseLeafLease() noexcept
{
    release();
}

DatabaseLeafLease::DatabaseLeafLease(DatabaseLeafLease&& other) noexcept
    : role_{other.role_}, handle_{std::move(other.handle_)}, identity_{other.identity_},
      wasCreated_{std::exchange(other.wasCreated_, false)}, state_{std::move(other.state_)}
{
}

DatabaseLeafLease& DatabaseLeafLease::operator=(DatabaseLeafLease&& other) noexcept
{
    if (this != &other) {
        release();
        role_ = other.role_;
        handle_ = std::move(other.handle_);
        identity_ = other.identity_;
        wasCreated_ = std::exchange(other.wasCreated_, false);
        state_ = std::move(other.state_);
    }
    return *this;
}

void DatabaseLeafLease::release() noexcept
{
    if (state_ != nullptr && handle_) {
        releaseCohortIdentity(*state_, role_, identity_);
    }
    handle_.reset();
    state_.reset();
    wasCreated_ = false;
}

DatabaseMigrationLock::DatabaseMigrationLock(
    DatabaseLeafLease pinnedLeaf,
    InfrastructureDetail::UniqueHandle lockHandle) noexcept
    : pinnedLeaf_{std::move(pinnedLeaf)},
      lockHandle_{std::move(lockHandle)},
      locked_{true}
{
}

DatabaseMigrationLock::~DatabaseMigrationLock() noexcept
{
    release();
}

DatabaseMigrationLock::DatabaseMigrationLock(DatabaseMigrationLock&& other) noexcept
    : pinnedLeaf_{std::move(other.pinnedLeaf_)},
      lockHandle_{std::move(other.lockHandle_)},
      locked_{std::exchange(other.locked_, false)}
{
}

DatabaseMigrationLock& DatabaseMigrationLock::operator=(DatabaseMigrationLock&& other) noexcept
{
    if (this != &other) {
        release();
        pinnedLeaf_ = std::move(other.pinnedLeaf_);
        lockHandle_ = std::move(other.lockHandle_);
        locked_ = std::exchange(other.locked_, false);
    }
    return *this;
}

void DatabaseMigrationLock::release() noexcept
{
    if (locked_) {
        unlockByteZero(lockHandle_.get());
    }
    locked_ = false;
    lockHandle_.reset();
    pinnedLeaf_ = DatabaseLeafLease{};
}

DatabaseNamespaceLease::DatabaseNamespaceLease(
    std::shared_ptr<DatabaseNamespaceState> state) noexcept
    : state_{std::move(state)}
{
}

Domain::Result<std::shared_ptr<DatabaseNamespaceLease>> DatabaseNamespaceLease::create(
    const std::wstring_view canonicalDirectory,
    const std::wstring_view mainBasename,
    const std::wstring_view migrationLockBasename) noexcept
{
    try {
        auto validatedDirectory = validateCanonicalDirectory(canonicalDirectory);
        if (!validatedDirectory) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(
                std::move(validatedDirectory).error());
        }
        if (!validLeafName(mainBasename) || !validLeafName(migrationLockBasename) ||
            mainBasename.size() + std::wstring_view{L"-journal"}.size() > MaximumLeafNameCharacters) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Database and migration-lock basenames must be distinct safe fixed leaf names."));
        }

        std::array<std::wstring, 4U> sqliteLeafNames{
            std::wstring{mainBasename},
            std::wstring{mainBasename} + L"-wal",
            std::wstring{mainBasename} + L"-shm",
            std::wstring{mainBasename} + L"-journal"};
        const bool lockCollision = std::any_of(
            sqliteLeafNames.begin(), sqliteLeafNames.end(),
            [migrationLockBasename](const std::wstring& leaf) noexcept {
                return equalInsensitive(leaf, migrationLockBasename);
            });
        if (lockCollision) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "The migration-lock basename collides with a fixed SQLite database leaf."));
        }
        const std::size_t longestLeaf = (std::max)(
            sqliteLeafNames.back().size(), migrationLockBasename.size());
        if (validatedDirectory.value().size() >
            MaximumNativePathCharacters - 1U - longestLeaf) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(namespaceError(
                Domain::ErrorCodes::LimitExceeded,
                "The canonical database namespace exceeds the bounded native path limit."));
        }

        auto anchored = createAndAnchorDirectoryTree(validatedDirectory.value());
        if (!anchored) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(
                std::move(anchored).error());
        }
        auto directoryIdentity = fileIdentity(
            anchored.value().handles.back().get(), "read the database directory identity");
        if (!directoryIdentity) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(
                std::move(directoryIdentity).error());
        }

        auto state = std::make_shared<DatabaseNamespaceState>();
        state->canonicalDirectory = std::move(validatedDirectory).value();
        state->leafNames[roleIndex(DatabaseLeafRole::Main)] = std::move(sqliteLeafNames[0U]);
        state->leafNames[roleIndex(DatabaseLeafRole::Wal)] = std::move(sqliteLeafNames[1U]);
        state->leafNames[roleIndex(DatabaseLeafRole::SharedMemory)] =
            std::move(sqliteLeafNames[2U]);
        state->leafNames[roleIndex(DatabaseLeafRole::Journal)] =
            std::move(sqliteLeafNames[3U]);
        state->leafNames[roleIndex(DatabaseLeafRole::MigrationLock)] =
            std::wstring{migrationLockBasename};

        for (std::size_t index = 0U; index < state->leafNames.size(); ++index) {
            state->canonicalPaths[index] = state->canonicalDirectory + L"\\" + state->leafNames[index];
            auto utf8 = strictUtf16ToUtf8(state->canonicalPaths[index]);
            if (!utf8) {
                return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(
                    std::move(utf8).error());
            }
            state->canonicalUtf8Paths[index] = std::move(utf8).value();
        }
        state->anchorPaths = std::move(anchored).value().paths;
        state->anchors = std::move(anchored).value().handles;
        state->directoryIdentity = directoryIdentity.value();

        auto lease = std::shared_ptr<DatabaseNamespaceLease>(
            new DatabaseNamespaceLease{std::move(state)});
        auto revalidated = lease->revalidate();
        if (!revalidated) {
            return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(
                std::move(revalidated).error());
        }
        return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::success(std::move(lease));
    } catch (...) {
        return Domain::Result<std::shared_ptr<DatabaseNamespaceLease>>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database namespace could not be created and anchored."));
    }
}

const std::wstring& DatabaseNamespaceLease::canonicalDirectory() const noexcept
{
    return state_->canonicalDirectory;
}

const std::wstring& DatabaseNamespaceLease::canonicalMainDatabasePath() const noexcept
{
    return state_->canonicalPaths[roleIndex(DatabaseLeafRole::Main)];
}

const std::wstring& DatabaseNamespaceLease::leafName(const DatabaseLeafRole role) const noexcept
{
    return state_->leafNames[roleIndex(role)];
}

const std::wstring& DatabaseNamespaceLease::canonicalPath(const DatabaseLeafRole role) const noexcept
{
    return state_->canonicalPaths[roleIndex(role)];
}

const std::string& DatabaseNamespaceLease::canonicalUtf8Path(const DatabaseLeafRole role) const noexcept
{
    return state_->canonicalUtf8Paths[roleIndex(role)];
}

Domain::Result<DatabaseLeafRole> DatabaseNamespaceLease::classifyCanonicalPath(
    const std::wstring_view candidate) const noexcept
{
    try {
        for (std::size_t index = 0U; index < state_->canonicalPaths.size(); ++index) {
            if (equalInsensitive(candidate, state_->canonicalPaths[index])) {
                return Domain::Result<DatabaseLeafRole>::success(
                    static_cast<DatabaseLeafRole>(index));
            }
        }
        return Domain::Result<DatabaseLeafRole>::failure(namespaceError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "SQLite requested a path outside its fixed database namespace."));
    } catch (...) {
        return Domain::Result<DatabaseLeafRole>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A SQLite path could not be classified in its database namespace."));
    }
}

Domain::Result<DatabaseLeafLease> DatabaseNamespaceLease::openLeaf(
    const DatabaseLeafRole role,
    const DatabaseLeafDisposition disposition,
    const ACCESS_MASK desiredAccess) const noexcept
{
    const ULONG shareAccess = role == DatabaseLeafRole::MigrationLock
                                  ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                                  : FILE_SHARE_READ | FILE_SHARE_WRITE;
    return openLeafWithShareAccess(
        role,
        disposition,
        desiredAccess,
        shareAccess);
}

Domain::Result<DatabaseLeafLease> DatabaseNamespaceLease::openLeafWithShareAccess(
    const DatabaseLeafRole role,
    const DatabaseLeafDisposition disposition,
    const ACCESS_MASK desiredAccess,
    const ULONG shareAccess) const noexcept
{
    try {
        constexpr ACCESS_MASK PermittedLeafAccess =
            GENERIC_READ | GENERIC_WRITE |
            FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA |
            FILE_READ_EA | FILE_WRITE_EA |
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
            READ_CONTROL | SYNCHRONIZE;
        const bool validDisposition = disposition == DatabaseLeafDisposition::OpenExisting ||
                                      disposition == DatabaseLeafDisposition::CreateNew ||
                                      disposition == DatabaseLeafDisposition::OpenOrCreate;
        const bool validShareAccess = shareAccess == FILE_SHARE_READ ||
                                      shareAccess ==
                                          (FILE_SHARE_READ | FILE_SHARE_WRITE) ||
                                      shareAccess ==
                                          (FILE_SHARE_READ | FILE_SHARE_WRITE |
                                           FILE_SHARE_DELETE);
        if (desiredAccess == 0U || (desiredAccess & ~PermittedLeafAccess) != 0U ||
            !validRole(role) || !validDisposition || !validShareAccess) {
            return Domain::Result<DatabaseLeafLease>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database leaf open request has invalid access, sharing, or disposition."));
        }
        auto validNamespace = revalidate();
        if (!validNamespace) {
            return Domain::Result<DatabaseLeafLease>::failure(
                std::move(validNamespace).error());
        }

        InfrastructureDetail::RelativeOpenOptions options{};
        options.desiredAccess = desiredAccess;
        options.shareAccess = shareAccess;
        options.disposition = nativeDisposition(disposition);
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = InfrastructureDetail::RelativeObjectType::File;
        auto opened = InfrastructureDetail::openRelative(
            state_->anchors.back().get(), leafName(role), options);
        if (!opened) {
            const bool missing = missingFileError(opened.win32Error);
            const bool createCollision =
                disposition == DatabaseLeafDisposition::CreateNew &&
                (opened.win32Error == ERROR_FILE_EXISTS ||
                 opened.win32Error == ERROR_ALREADY_EXISTS);
            return Domain::Result<DatabaseLeafLease>::failure(fileError(
                "open a handle-relative database leaf",
                opened.win32Error,
                missing ? Domain::ErrorCodes::RecordNotFound
                        : createCollision ? Domain::ErrorCodes::Conflict
                                          : Domain::ErrorCodes::InternalFailure,
                createCollision || opened.win32Error == ERROR_SHARING_VIOLATION ||
                    opened.win32Error == ERROR_LOCK_VIOLATION));
        }
        auto identity = verifyFileHandle(opened.handle.get(), canonicalPath(role));
        if (!identity) {
            return Domain::Result<DatabaseLeafLease>::failure(std::move(identity).error());
        }
        auto remembered = retainCohortIdentity(*state_, role, identity.value());
        if (!remembered) {
            return Domain::Result<DatabaseLeafLease>::failure(
                std::move(remembered).error());
        }
        return Domain::Result<DatabaseLeafLease>::success(DatabaseLeafLease{
            role, std::move(opened.handle), identity.value(), opened.wasCreated(), state_});
    } catch (...) {
        return Domain::Result<DatabaseLeafLease>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A database leaf could not be securely opened."));
    }
}

Domain::Result<bool> DatabaseNamespaceLease::accessLeaf(
    const DatabaseLeafRole role,
    const DatabaseLeafAccess access) const noexcept
{
    try {
        const bool validAccess = access == DatabaseLeafAccess::Exists ||
                                 access == DatabaseLeafAccess::Read ||
                                 access == DatabaseLeafAccess::ReadWrite;
        if (!validRole(role) || !validAccess) {
            return Domain::Result<bool>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database leaf access request is invalid."));
        }
        auto validNamespace = revalidate();
        if (!validNamespace) {
            return Domain::Result<bool>::failure(std::move(validNamespace).error());
        }
        ACCESS_MASK desiredAccess = FILE_READ_ATTRIBUTES;
        if (access == DatabaseLeafAccess::Read) {
            desiredAccess |= FILE_READ_DATA;
        } else if (access == DatabaseLeafAccess::ReadWrite) {
            desiredAccess |= FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_EA | FILE_WRITE_EA;
        }

        InfrastructureDetail::RelativeOpenOptions options{};
        options.desiredAccess = desiredAccess;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = InfrastructureDetail::RelativeObjectType::File;
        auto opened = InfrastructureDetail::openRelative(
            state_->anchors.back().get(), leafName(role), options);
        if (!opened) {
            if (missingFileError(opened.win32Error)) {
                return Domain::Result<bool>::success(false);
            }
            return Domain::Result<bool>::failure(fileError(
                "inspect a handle-relative database leaf",
                opened.win32Error,
                Domain::ErrorCodes::InternalFailure,
                opened.win32Error == ERROR_SHARING_VIOLATION));
        }
        auto identity = verifyFileHandle(opened.handle.get(), canonicalPath(role));
        if (!identity) {
            return Domain::Result<bool>::failure(std::move(identity).error());
        }
        auto remembered = rememberCohortIdentity(*state_, role, identity.value());
        if (!remembered) {
            return Domain::Result<bool>::failure(std::move(remembered).error());
        }
        return Domain::Result<bool>::success(true);
    } catch (...) {
        return Domain::Result<bool>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A database leaf access check failed."));
    }
}

Domain::Result<std::vector<DatabaseLeafLease>>
DatabaseNamespaceLease::captureClosedCohortForQuarantine() const noexcept
{
    try {
        if (!state_ || state_->anchors.empty()) {
            return Domain::Result<std::vector<DatabaseLeafLease>>::failure(namespaceError(
                Domain::ErrorCodes::IntegrityFailure,
                "Database quarantine has no retained namespace authority."));
        }
        if (openVfsFileCount() != 0U) {
            return Domain::Result<std::vector<DatabaseLeafLease>>::failure(namespaceError(
                Domain::ErrorCodes::DatabaseBusy,
                "Database quarantine requires every SQLite VFS file to be closed.",
                true));
        }
        auto validAnchors = revalidateNamespaceAnchors(*state_);
        if (!validAnchors) {
            return Domain::Result<std::vector<DatabaseLeafLease>>::failure(
                std::move(validAnchors).error());
        }

        std::vector<DatabaseLeafLease> captured;
        captured.reserve(QuarantineCohortRoles.size());
        for (const DatabaseLeafRole role : QuarantineCohortRoles) {
            InfrastructureDetail::RelativeOpenOptions options{};
            options.desiredAccess = GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES;
            options.shareAccess = FILE_SHARE_READ;
            options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
            options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
            options.objectType = InfrastructureDetail::RelativeObjectType::File;
            const std::size_t index = roleIndex(role);
            auto opened = InfrastructureDetail::openRelative(
                state_->anchors.back().get(), state_->leafNames[index], options);
            if (!opened) {
                if (role != DatabaseLeafRole::Main && missingFileError(opened.win32Error)) {
                    continue;
                }
                return Domain::Result<std::vector<DatabaseLeafLease>>::failure(fileError(
                    "capture an exact closed database cohort leaf for quarantine",
                    opened.win32Error,
                    opened.win32Error == ERROR_SHARING_VIOLATION ||
                            opened.win32Error == ERROR_LOCK_VIOLATION
                        ? Domain::ErrorCodes::DatabaseBusy
                        : Domain::ErrorCodes::InternalFailure,
                    opened.win32Error == ERROR_SHARING_VIOLATION ||
                        opened.win32Error == ERROR_LOCK_VIOLATION));
            }
            auto identity = verifyFileHandle(opened.handle.get(), state_->canonicalPaths[index]);
            if (!identity) {
                return Domain::Result<std::vector<DatabaseLeafLease>>::failure(
                    std::move(identity).error());
            }
            auto retained = retainCohortIdentity(*state_, role, identity.value());
            if (!retained) {
                return Domain::Result<std::vector<DatabaseLeafLease>>::failure(
                    std::move(retained).error());
            }
            captured.push_back(DatabaseLeafLease{
                role, std::move(opened.handle), identity.value(), false, state_});
        }
        if (captured.empty() || captured.front().role() != DatabaseLeafRole::Main) {
            return Domain::Result<std::vector<DatabaseLeafLease>>::failure(namespaceError(
                Domain::ErrorCodes::RecordNotFound,
                "Database quarantine requires a source main database file."));
        }

        std::vector<const DatabaseLeafLease*> expected;
        expected.reserve(captured.size());
        for (const auto& leaf : captured) {
            expected.push_back(std::addressof(leaf));
        }
        auto exact = revalidateExactQuarantineCohort(expected);
        if (!exact) {
            return Domain::Result<std::vector<DatabaseLeafLease>>::failure(
                std::move(exact).error());
        }
        return Domain::Result<std::vector<DatabaseLeafLease>>::success(
            std::move(captured));
    } catch (...) {
        return Domain::Result<std::vector<DatabaseLeafLease>>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The closed database cohort could not be captured for quarantine."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::revalidateExactQuarantineCohort(
    const std::span<const DatabaseLeafLease* const> expectedLeaves) const noexcept
{
    try {
        if (!state_ || state_->anchors.empty() || expectedLeaves.empty() ||
            expectedLeaves.size() > QuarantineCohortRoles.size()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Exact quarantine-cohort validation requires one through four retained leaves."));
        }
        if (openVfsFileCount() != 0U) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::DatabaseBusy,
                "Exact quarantine-cohort validation requires a closed SQLite namespace.",
                true));
        }
        auto validAnchors = revalidateNamespaceAnchors(*state_);
        if (!validAnchors) {
            return validAnchors;
        }

        std::array<const DatabaseLeafLease*, QuarantineCohortRoles.size()> byRole{};
        for (const DatabaseLeafLease* const leaf : expectedLeaves) {
            if (leaf == nullptr || !*leaf || leaf->state_.get() != state_.get() ||
                leaf->role_ == DatabaseLeafRole::MigrationLock) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Exact quarantine-cohort validation received a foreign retained leaf."));
            }
            const std::size_t index = roleIndex(leaf->role_);
            if (index >= byRole.size() || byRole[index] != nullptr) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Exact quarantine-cohort validation received a duplicate or invalid role."));
            }
            byRole[index] = leaf;
        }
        if (byRole[roleIndex(DatabaseLeafRole::Main)] == nullptr) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Exact quarantine-cohort validation requires the retained main database."));
        }

        for (const DatabaseLeafRole role : QuarantineCohortRoles) {
            const std::size_t index = roleIndex(role);
            const DatabaseLeafLease* const expected = byRole[index];
            if (expected != nullptr) {
                auto identity = verifyFileHandle(
                    expected->handle_.get(), state_->canonicalPaths[index]);
                if (!identity) {
                    return Domain::Result<void>::failure(std::move(identity).error());
                }
                if (identity.value() != expected->identity_) {
                    return Domain::Result<void>::failure(namespaceError(
                        Domain::ErrorCodes::Conflict,
                        "A retained quarantine-cohort leaf changed identity.",
                        true));
                }
                continue;
            }

            InfrastructureDetail::RelativeOpenOptions options{};
            options.desiredAccess = FILE_READ_ATTRIBUTES;
            options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
            options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
            options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
            options.objectType = InfrastructureDetail::RelativeObjectType::File;
            auto observed = InfrastructureDetail::openRelative(
                state_->anchors.back().get(), state_->leafNames[index], options);
            if (!observed) {
                if (missingFileError(observed.win32Error)) {
                    continue;
                }
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "An absent quarantine-cohort role became inaccessible during exact validation.",
                    true));
            }
            auto identity = verifyFileHandle(
                observed.handle.get(), state_->canonicalPaths[index]);
            if (!identity) {
                return Domain::Result<void>::failure(std::move(identity).error());
            }
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "An absent quarantine-cohort role appeared during exact validation.",
                true));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The exact quarantine cohort could not be revalidated."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::deleteRetainedQuarantineLeaf(
    DatabaseLeafLease& leaf) const noexcept
{
    try {
        if (!state_ || !leaf || leaf.state_.get() != state_.get() ||
            leaf.role_ == DatabaseLeafRole::MigrationLock) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Quarantine deletion requires an exact retained source-cohort leaf."));
        }
        auto validAnchors = revalidateNamespaceAnchors(*state_);
        if (!validAnchors) {
            return validAnchors;
        }
        auto identity = verifyFileHandle(
            leaf.handle_.get(), canonicalPath(leaf.role_));
        if (!identity) {
            return Domain::Result<void>::failure(std::move(identity).error());
        }
        if (identity.value() != leaf.identity_) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "The retained quarantine source changed identity before deletion.",
                true));
        }

        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
                            FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                            FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        if (::SetFileInformationByHandle(
                leaf.handle_.get(), FileDispositionInfoEx,
                &disposition, sizeof(disposition)) == FALSE) {
            return Domain::Result<void>::failure(fileError(
                "delete an exact retained quarantine source leaf",
                ::GetLastError(), Domain::ErrorCodes::InternalFailure, true));
        }
        const DatabaseLeafRole role = leaf.role_;
        const DatabaseFileIdentity deletedIdentity = leaf.identity_;
        leaf = DatabaseLeafLease{};
        clearCohortIdentity(*state_, role, deletedIdentity);
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The exact retained quarantine source could not be deleted."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::deleteTransientLeaf(
    const DatabaseLeafRole role) const noexcept
{
    if (role != DatabaseLeafRole::Wal && role != DatabaseLeafRole::SharedMemory &&
        role != DatabaseLeafRole::Journal) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Unauthorized,
            "SQLite may delete only its fixed WAL, shared-memory, and rollback-journal leaves."));
    }
    return deleteLeaf(role, nullptr, true);
}

Domain::Result<void> DatabaseNamespaceLease::discardCreatedLeaf(
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& expectedIdentity) const noexcept
{
    if (!validRole(role) || role == DatabaseLeafRole::MigrationLock) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Unauthorized,
            "A VFS open failure may not discard the migration-lock leaf."));
    }
    return deleteLeaf(role, &expectedIdentity, true);
}

Domain::Result<void> DatabaseNamespaceLease::deleteClosedLeaf(
    const DatabaseLeafRole role,
    const DatabaseFileIdentity& expectedIdentity) const noexcept
{
    if (!validRole(role) || role == DatabaseLeafRole::MigrationLock) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Unauthorized,
            "Closed-cohort deletion permits only an exact SQLite database leaf."));
    }
    if (openVfsFileCount() != 0U) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::DatabaseBusy,
            "Closed-cohort deletion requires the namespace to have no open VFS files.",
            true));
    }
    return deleteLeaf(role, &expectedIdentity, false);
}

Domain::Result<void> DatabaseNamespaceLease::deleteLeaf(
    const DatabaseLeafRole role,
    const DatabaseFileIdentity* const expectedIdentity,
    const bool missingIsSuccess) const noexcept
{
    try {
        if (!validRole(role)) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A database leaf deletion request has an invalid role."));
        }
        auto validNamespace = revalidate();
        if (!validNamespace) {
            return validNamespace;
        }
        InfrastructureDetail::RelativeOpenOptions options{};
        options.desiredAccess = DELETE | FILE_READ_ATTRIBUTES;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = InfrastructureDetail::RelativeObjectType::File;
        auto opened = InfrastructureDetail::openRelative(
            state_->anchors.back().get(), leafName(role), options);
        if (!opened) {
            if (missingIsSuccess && missingFileError(opened.win32Error)) {
                return Domain::Result<void>::success();
            }
            return Domain::Result<void>::failure(fileError(
                "open a database leaf for handle-relative deletion",
                opened.win32Error,
                opened.win32Error == ERROR_SHARING_VIOLATION
                    ? Domain::ErrorCodes::DatabaseBusy
                    : Domain::ErrorCodes::InternalFailure,
                opened.win32Error == ERROR_SHARING_VIOLATION));
        }
        auto identity = verifyFileHandle(opened.handle.get(), canonicalPath(role));
        if (!identity) {
            return Domain::Result<void>::failure(std::move(identity).error());
        }
        if (expectedIdentity != nullptr && identity.value() != *expectedIdentity) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "A database leaf identity changed before secure deletion.",
                true));
        }
        auto remembered = rememberCohortIdentity(*state_, role, identity.value());
        if (!remembered) {
            return remembered;
        }

        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
                            FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                            FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        if (::SetFileInformationByHandle(
                opened.handle.get(), FileDispositionInfoEx,
                &disposition, sizeof(disposition)) == FALSE) {
            return Domain::Result<void>::failure(fileError(
                "delete a handle-relative database leaf",
                ::GetLastError(), Domain::ErrorCodes::InternalFailure, true));
        }
        clearCohortIdentity(*state_, role, identity.value());
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A database leaf could not be securely deleted."));
    }
}

Domain::Result<DatabaseMigrationLock> DatabaseNamespaceLease::acquireMigrationLock(
    const Domain::OperationContext& context) const noexcept
{
    try {
        const auto admissionStarted = std::chrono::steady_clock::now();
        const auto busyDeadline = admissionStarted + MaximumMigrationLockWait;
        const bool contextDeadlineWins = context.deadline <= busyDeadline;
        const auto effectiveDeadline = contextDeadlineWins
            ? context.deadline
            : busyDeadline;
        const auto admissionFailure = [&]() -> Domain::Result<DatabaseMigrationLock> {
            if (context.isCancellationRequested()) {
                return Domain::Result<DatabaseMigrationLock>::failure(
                    operationInterruption(context, "The database migration lock"));
            }
            if (contextDeadlineWins) {
                return Domain::Result<DatabaseMigrationLock>::failure(namespaceError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The database migration lock exceeded its selected operation deadline."));
            }
            return Domain::Result<DatabaseMigrationLock>::failure(namespaceError(
                Domain::ErrorCodes::DatabaseBusy,
                "The database migration lock remained busy for three seconds.",
                true));
        };
        auto validContext = checkOperation(context, "The database migration lock");
        if (!validContext) {
            return Domain::Result<DatabaseMigrationLock>::failure(
                std::move(validContext).error());
        }
        InfrastructureDetail::UniqueHandle cancellationEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!cancellationEvent) {
            return Domain::Result<DatabaseMigrationLock>::failure(
                fileError("create a database lock cancellation event", ::GetLastError()));
        }
        std::stop_callback cancellationWake{
            context.cancellation,
            [event = cancellationEvent.get()]() noexcept {
                static_cast<void>(::SetEvent(event));
            }};
        for (;;) {
            validContext = checkOperation(context, "The database migration lock");
            if (!validContext || std::chrono::steady_clock::now() >= effectiveDeadline) {
                return admissionFailure();
            }

            auto pinned = openLeaf(
                DatabaseLeafRole::MigrationLock,
                DatabaseLeafDisposition::OpenOrCreate,
                GENERIC_READ | GENERIC_WRITE);
            if (!pinned) {
                if (pinned.error().code == Domain::ErrorCodes::PathOutsideAuthority) {
                    continue;
                }
                return Domain::Result<DatabaseMigrationLock>::failure(
                    std::move(pinned).error());
            }

            InfrastructureDetail::UniqueHandle lockHandle{::ReOpenFile(
                pinned.value().nativeHandle(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_FLAG_OPEN_REPARSE_POINT)};
            if (!lockHandle) {
                return Domain::Result<DatabaseMigrationLock>::failure(fileError(
                    "reopen the database migration lock for bounded admission",
                    ::GetLastError()));
            }
            auto lockIdentity = fileIdentity(
                lockHandle.get(),
                "read the bounded database migration-lock identity");
            if (!lockIdentity) {
                return Domain::Result<DatabaseMigrationLock>::failure(
                    std::move(lockIdentity).error());
            }
            if (lockIdentity.value() != pinned.value().identity()) {
                return Domain::Result<DatabaseMigrationLock>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "The bounded migration-lock handle changed identity.",
                    true));
            }

            OVERLAPPED operation{};
            const bool acquired = ::LockFileEx(
                                      lockHandle.get(),
                                      LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                      0U, 1U, 0U, &operation) != FALSE;
            if (!acquired) {
                const DWORD lockError = ::GetLastError();
                if (lockError != ERROR_LOCK_VIOLATION) {
                    return Domain::Result<DatabaseMigrationLock>::failure(fileError(
                        "attempt a bounded database migration lock",
                        lockError,
                        Domain::ErrorCodes::InternalFailure));
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= effectiveDeadline || context.isCancellationRequested()) {
                    return admissionFailure();
                }
                const auto retryDeadline = (std::min)(
                    effectiveDeadline, now + MigrationLockRetrySlice);
                const DWORD retryWait = ::WaitForSingleObject(
                    cancellationEvent.get(), waitMillisecondsUntil(retryDeadline));
                if (retryWait == WAIT_OBJECT_0) {
                    return admissionFailure();
                }
                if (retryWait != WAIT_TIMEOUT) {
                    return Domain::Result<DatabaseMigrationLock>::failure(fileError(
                        "wait to retry a contended database migration lock",
                        retryWait == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_FUNCTION));
                }
                continue;
            }

            const auto admittedAt = std::chrono::steady_clock::now();
            if (context.isCancellationRequested() || admittedAt >= effectiveDeadline) {
                unlockByteZero(lockHandle.get());
                return admissionFailure();
            }

            // Publication removes an ephemeral lock name before releasing its byte lock.
            // A waiter may therefore be admitted on the now-unlinked predecessor. Only
            // return ownership when the anchored pathname still names this exact file;
            // otherwise release the stale byte lock and retry against the current name.
            auto currentLock = openLeaf(
                DatabaseLeafRole::MigrationLock,
                DatabaseLeafDisposition::OpenExisting,
                FILE_READ_ATTRIBUTES);
            if (!currentLock) {
                unlockByteZero(lockHandle.get());
                if (currentLock.error().code == Domain::ErrorCodes::RecordNotFound ||
                    currentLock.error().code == Domain::ErrorCodes::PathOutsideAuthority) {
                    continue;
                }
                return Domain::Result<DatabaseMigrationLock>::failure(
                    std::move(currentLock).error());
            }
            if (currentLock.value().identity() != pinned.value().identity()) {
                unlockByteZero(lockHandle.get());
                continue;
            }
            return Domain::Result<DatabaseMigrationLock>::success(DatabaseMigrationLock{
                std::move(pinned).value(), std::move(lockHandle)});
        }
    } catch (...) {
        return Domain::Result<DatabaseMigrationLock>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database migration lock could not be acquired."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::revalidate() const noexcept
{
    try {
        if (!state_) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database namespace has incomplete directory anchors."));
        }
        auto validAnchors = revalidateNamespaceAnchors(*state_);
        if (!validAnchors) {
            return validAnchors;
        }
        return revalidateCohort();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database namespace could not be revalidated."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::revalidateRetainedLeaf(
    const DatabaseLeafLease& leaf) const noexcept
{
    try {
        if (!state_ || !leaf || leaf.role() != DatabaseLeafRole::Main) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Retained database revalidation requires an exact main leaf."));
        }
        auto validAnchors = revalidateNamespaceAnchors(*state_);
        if (!validAnchors) {
            return validAnchors;
        }

        auto retainedBefore = verifyFileHandle(
            leaf.nativeHandle(), canonicalPath(leaf.role()));
        if (!retainedBefore) {
            return Domain::Result<void>::failure(std::move(retainedBefore).error());
        }
        if (retainedBefore.value() != leaf.identity()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "A retained database leaf changed identity.",
                true));
        }

        // The retained Main handle denies writes and namespace replacement.
        // Sidecars must remain absent for the standalone backup; do not adopt a
        // role injected after publication as part of the expected cohort.
        std::scoped_lock lock{state_->cohortMutex};
        auto& expectedMain = state_->expectedCohortIdentities[0U];
        if (expectedMain.has_value() && *expectedMain != leaf.identity()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "The retained main database differs from its remembered identity.",
                true));
        }
        expectedMain = leaf.identity();
        constexpr std::array StrictlyAbsentRoles{
            DatabaseLeafRole::Wal,
            DatabaseLeafRole::SharedMemory,
            DatabaseLeafRole::Journal};
        for (const DatabaseLeafRole role : StrictlyAbsentRoles) {
            auto current = inspectCohortLeaf(*state_, role);
            if (!current) {
                return Domain::Result<void>::failure(std::move(current).error());
            }
            if (current.value().has_value()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "A standalone retained database acquired a SQLite sidecar.",
                    true));
            }
            const auto expectedIndex = cohortIdentityIndex(role);
            if (expectedIndex.has_value() &&
                state_->expectedCohortIdentities[*expectedIndex].has_value()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "A remembered standalone database sidecar disappeared.",
                    true));
            }
        }

        auto retainedAfter = verifyFileHandle(
            leaf.nativeHandle(), canonicalPath(leaf.role()));
        if (!retainedAfter) {
            return Domain::Result<void>::failure(std::move(retainedAfter).error());
        }
        if (retainedAfter.value() != leaf.identity()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "A retained database leaf changed during revalidation.",
                true));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A retained database leaf could not be revalidated."));
    }
}

Domain::Result<void> DatabaseNamespaceLease::revalidateCohort() const noexcept
{
    try {
        if (!state_ || state_->anchors.empty()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database cohort has no retained directory anchor."));
        }
        std::scoped_lock lock{state_->cohortMutex};
        std::array<std::optional<CohortIdentityObservation>, 3U> observations;
        for (std::size_t roleOffset = 0U;
             roleOffset < IdentityTrackedCohortRoles.size(); ++roleOffset) {
            const DatabaseLeafRole role = IdentityTrackedCohortRoles[roleOffset];
            auto current = inspectCohortLeaf(*state_, role);
            if (!current) {
                return Domain::Result<void>::failure(std::move(current).error());
            }
            observations[roleOffset] = std::move(current).value();
        }
        for (std::size_t roleOffset = 0U;
             roleOffset < IdentityTrackedCohortRoles.size(); ++roleOffset) {
            const DatabaseLeafRole role = IdentityTrackedCohortRoles[roleOffset];
            const auto index = cohortIdentityIndex(role);
            if (!index.has_value()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::InternalFailure,
                    "The database cohort identity map is invalid."));
            }
            auto& expected = state_->expectedCohortIdentities[*index];
            if (!observations[roleOffset].has_value()) {
                if (expected.has_value()) {
                    if (role == DatabaseLeafRole::Main ||
                        state_->retainedCohortOwners[*index] != 0U) {
                        return Domain::Result<void>::failure(namespaceError(
                            Domain::ErrorCodes::Conflict,
                            "A retained database cohort leaf disappeared.",
                            true));
                    }
                    expected.reset();
                }
                continue;
            }
            const DatabaseFileIdentity& currentIdentity =
                observations[roleOffset]->identity;
            if (expected.has_value() && *expected != currentIdentity) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "A database cohort leaf changed from its remembered identity.",
                    true));
            }
            if (role == DatabaseLeafRole::Main ||
                state_->retainedCohortOwners[*index] != 0U) {
                expected = currentIdentity;
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The database cohort identities could not be revalidated."));
    }
}

Domain::Result<std::vector<std::wstring>> DatabaseNamespaceLease::enumerateMatchingLeafNames(
    const std::wstring_view requiredPrefix,
    const std::wstring_view requiredSuffix,
    const std::size_t maximumResults,
    const Domain::OperationContext& context) const noexcept
{
    try {
        if (!validLeafFragment(requiredPrefix) || !validLeafFragment(requiredSuffix) ||
            requiredPrefix.size() + requiredSuffix.size() > MaximumLeafNameCharacters ||
            maximumResults == 0U || maximumResults > MaximumMatchingLeafNames) {
            return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A bounded leaf enumeration requires safe literal fragments and 1 through 32 results."));
        }
        auto validContext = checkOperation(context, "Database leaf enumeration");
        if (!validContext) {
            return Domain::Result<std::vector<std::wstring>>::failure(
                std::move(validContext).error());
        }
        auto validNamespace = revalidate();
        if (!validNamespace) {
            return Domain::Result<std::vector<std::wstring>>::failure(
                std::move(validNamespace).error());
        }

        std::scoped_lock enumerationLock{state_->enumerationMutex};
        alignas(FILE_ID_BOTH_DIR_INFO)
            std::array<std::byte, 16U * 1024U> buffer{};
        std::vector<std::wstring> result;
        result.reserve(maximumResults);
        std::size_t inspected{};
        FILE_INFO_BY_HANDLE_CLASS informationClass = FileIdBothDirectoryRestartInfo;
        for (;;) {
            validContext = checkOperation(context, "Database leaf enumeration");
            if (!validContext) {
                return Domain::Result<std::vector<std::wstring>>::failure(
                    std::move(validContext).error());
            }
            const BOOL queried = ::GetFileInformationByHandleEx(
                state_->anchors.back().get(), informationClass,
                buffer.data(), static_cast<DWORD>(buffer.size()));
            if (queried == FALSE) {
                const DWORD queryError = ::GetLastError();
                if (queryError == ERROR_NO_MORE_FILES) {
                    break;
                }
                return Domain::Result<std::vector<std::wstring>>::failure(fileError(
                    "enumerate anchored database directory leaves", queryError));
            }
            informationClass = FileIdBothDirectoryInfo;

            std::size_t offset{};
            for (;;) {
                constexpr std::size_t HeaderBytes =
                    offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > buffer.size() || buffer.size() - offset < HeaderBytes) {
                    return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "A native database directory enumeration record is truncated."));
                }
                const auto* const information =
                    reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                const std::size_t nameBytes = information->FileNameLength;
                if ((nameBytes % sizeof(wchar_t)) != 0U ||
                    nameBytes > buffer.size() - offset - HeaderBytes) {
                    return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "A native database directory filename is malformed."));
                }
                ++inspected;
                if (inspected > MaximumDirectoryEntriesInspected) {
                    return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                        Domain::ErrorCodes::LimitExceeded,
                        "A database directory enumeration exceeded 4096 direct entries."));
                }

                const std::wstring_view name{
                    information->FileName, nameBytes / sizeof(wchar_t)};
                if (name != L"." && name != L".." &&
                    matchesLeafFragments(name, requiredPrefix, requiredSuffix)) {
                    if (!validLeafName(name) ||
                        (information->FileAttributes &
                         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
                        return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "A matching database leaf name is not a safe regular file."));
                    }
                    if (result.size() == maximumResults) {
                        return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                            Domain::ErrorCodes::LimitExceeded,
                            "A database leaf enumeration exceeded its caller result bound."));
                    }
                    result.emplace_back(name);
                }

                if (information->NextEntryOffset == 0U) {
                    break;
                }
                const std::size_t nextOffset = information->NextEntryOffset;
                if (nextOffset < HeaderBytes || nextOffset > buffer.size() - offset) {
                    return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "A native database directory enumeration offset is malformed."));
                }
                offset += nextOffset;
            }
        }
        std::sort(result.begin(), result.end(), [](const std::wstring& left,
                                                   const std::wstring& right) noexcept {
            const int comparison = compareInsensitive(left, right);
            return comparison == 0 ? left < right : comparison < 0;
        });
        return Domain::Result<std::vector<std::wstring>>::success(std::move(result));
    } catch (...) {
        return Domain::Result<std::vector<std::wstring>>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored database directory could not be enumerated."));
    }
}

std::size_t DatabaseNamespaceLease::openVfsFileCount() const noexcept
{
    return state_ ? state_->openVfsFiles.load(std::memory_order_acquire) : 0U;
}

void DatabaseNamespaceLease::noteVfsFileOpened() noexcept
{
    if (state_) {
        state_->openVfsFiles.fetch_add(1U, std::memory_order_acq_rel);
    }
}

void DatabaseNamespaceLease::noteVfsFileClosed() noexcept
{
    if (!state_) {
        return;
    }
    std::size_t current = state_->openVfsFiles.load(std::memory_order_acquire);
    while (current != 0U &&
           !state_->openVfsFiles.compare_exchange_weak(
               current, current - 1U,
               std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

namespace {

struct StableDatabaseLeaf final {
    InfrastructureDetail::UniqueHandle handle;
    DatabaseFileIdentity identity{};
};

[[nodiscard]] Domain::Result<std::optional<StableDatabaseLeaf>> openStableLeaf(
    DatabaseNamespaceState& state,
    const DatabaseLeafRole role,
    const bool missingAllowed,
    const bool requireWriteAndDelete) noexcept
{
    try {
        InfrastructureDetail::RelativeOpenOptions options{};
        options.desiredAccess = requireWriteAndDelete
                                    ? GENERIC_READ | GENERIC_WRITE | DELETE
                                    : FILE_READ_DATA | FILE_READ_ATTRIBUTES;
        // Write sharing is intentionally denied. A source handle also denies delete
        // sharing so its verified identity cannot be renamed before publication.
        // A destination handle admits delete sharing only so that exact open target
        // can be replaced atomically by the source handle.
        options.shareAccess = requireWriteAndDelete
                                  ? FILE_SHARE_READ
                                  : FILE_SHARE_READ | FILE_SHARE_DELETE;
        options.disposition = InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = InfrastructureDetail::RelativeObjectType::File;
        const std::size_t index = roleIndex(role);
        auto opened = InfrastructureDetail::openRelative(
            state.anchors.back().get(), state.leafNames[index], options);
        if (!opened) {
            if (missingAllowed && missingFileError(opened.win32Error)) {
                return Domain::Result<std::optional<StableDatabaseLeaf>>::success(std::nullopt);
            }
            const bool missing = missingFileError(opened.win32Error);
            return Domain::Result<std::optional<StableDatabaseLeaf>>::failure(fileError(
                "open a closed database leaf for publication",
                opened.win32Error,
                missing ? Domain::ErrorCodes::RecordNotFound
                        : opened.win32Error == ERROR_SHARING_VIOLATION
                              ? Domain::ErrorCodes::DatabaseBusy
                              : Domain::ErrorCodes::InternalFailure,
                opened.win32Error == ERROR_SHARING_VIOLATION));
        }
        auto identity = verifyFileHandle(opened.handle.get(), state.canonicalPaths[index]);
        if (!identity) {
            return Domain::Result<std::optional<StableDatabaseLeaf>>::failure(
                std::move(identity).error());
        }
        auto remembered = rememberCohortIdentity(state, role, identity.value());
        if (!remembered) {
            return Domain::Result<std::optional<StableDatabaseLeaf>>::failure(
                std::move(remembered).error());
        }
        return Domain::Result<std::optional<StableDatabaseLeaf>>::success(
            StableDatabaseLeaf{std::move(opened.handle), identity.value()});
    } catch (...) {
        return Domain::Result<std::optional<StableDatabaseLeaf>>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A closed database leaf could not be opened for publication."));
    }
}

[[nodiscard]] Domain::Result<void> verifyStableLeaf(
    const StableDatabaseLeaf& leaf,
    const std::wstring_view expectedPath) noexcept
{
    auto current = verifyFileHandle(leaf.handle.get(), expectedPath);
    if (!current) {
        return Domain::Result<void>::failure(std::move(current).error());
    }
    if (current.value() != leaf.identity) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::Conflict,
            "A closed database leaf changed identity before publication.",
            true));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> renameStableLeaf(
    StableDatabaseLeaf& source,
    const std::wstring_view sourcePath,
    const DatabaseNamespaceState& destination,
    const DatabaseLeafRole destinationRole,
    const Domain::OperationContext& context,
    bool* const nativeRenameCompleted = nullptr) noexcept
{
    if (nativeRenameCompleted != nullptr) {
        *nativeRenameCompleted = false;
    }
    try {
        auto validContext = checkOperation(context, "Database leaf publication");
        if (!validContext) {
            return validContext;
        }
        auto verified = verifyStableLeaf(source, sourcePath);
        if (!verified) {
            return verified;
        }
        if (::FlushFileBuffers(source.handle.get()) == FALSE) {
            return Domain::Result<void>::failure(
                fileError("flush a closed database leaf before publication", ::GetLastError()));
        }
        verified = verifyStableLeaf(source, sourcePath);
        if (!verified) {
            return verified;
        }
        validContext = checkOperation(context, "Database leaf publication");
        if (!validContext) {
            return validContext;
        }

        const std::wstring& destinationName =
            destination.leafNames[roleIndex(destinationRole)];
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        if (informationBytes > (std::numeric_limits<DWORD>::max)()) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::LimitExceeded,
                "A database publication filename exceeds the native rename limit."));
        }
        std::vector<std::uint64_t> storage(
            (informationBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
        auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        std::memset(information, 0, informationBytes);
        information->Flags = 0U;
        // Native FileRenameInformationEx treats this simple name as relative to
        // the source handle's directory. Publication has already proved that the
        // destination namespace retains that same directory identity.
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';

        const DWORD renameError = renameRelativeToSourceDirectory(
            source.handle.get(), information, static_cast<ULONG>(informationBytes));
        if (renameError != ERROR_SUCCESS) {
            const bool destinationExists = renameError == ERROR_ALREADY_EXISTS ||
                                           renameError == ERROR_FILE_EXISTS;
            return Domain::Result<void>::failure(fileError(
                "publish a database leaf with source-handle-relative rename",
                renameError,
                destinationExists ? Domain::ErrorCodes::Conflict
                                  : Domain::ErrorCodes::InternalFailure,
                true));
        }
        if (nativeRenameCompleted != nullptr) {
            *nativeRenameCompleted = true;
        }

        const std::wstring& publishedPath =
            destination.canonicalPaths[roleIndex(destinationRole)];
        auto published = verifyFileHandle(source.handle.get(), publishedPath);
        if (!published) {
            return Domain::Result<void>::failure(std::move(published).error());
        }
        if (published.value() != source.identity) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::IntegrityFailure,
                "A published database leaf does not retain its staged identity."));
        }
        if (::FlushFileBuffers(source.handle.get()) == FALSE) {
            return Domain::Result<void>::failure(
                fileError("flush a published database leaf", ::GetLastError()));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A database leaf could not be published."));
    }
}

[[nodiscard]] Domain::Result<void> ensureClosedNamespaces(
    const DatabaseNamespaceLease& source,
    const DatabaseNamespaceLease& destination) noexcept
{
    if (source.openVfsFileCount() != 0U || destination.openVfsFileCount() != 0U) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::DatabaseBusy,
            "Database publication requires both namespace VFS owners to have no open files.",
            true));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> acquirePublicationLocks(
    DatabaseNamespaceLease& source,
    DatabaseNamespaceLease& destination,
    const Domain::OperationContext& context,
    std::optional<DatabaseMigrationLock>& first,
    std::optional<DatabaseMigrationLock>& second) noexcept
{
    const std::wstring& sourceLock = source.canonicalPath(DatabaseLeafRole::MigrationLock);
    const std::wstring& destinationLock = destination.canonicalPath(DatabaseLeafRole::MigrationLock);
    if (equalInsensitive(sourceLock, destinationLock)) {
        auto acquired = source.acquireMigrationLock(context);
        if (!acquired) {
            return Domain::Result<void>::failure(std::move(acquired).error());
        }
        first.emplace(std::move(acquired).value());
        return Domain::Result<void>::success();
    }

    DatabaseNamespaceLease* firstLease = &source;
    DatabaseNamespaceLease* secondLease = &destination;
    if (compareInsensitive(sourceLock, destinationLock) > 0) {
        std::swap(firstLease, secondLease);
    }
    auto firstResult = firstLease->acquireMigrationLock(context);
    if (!firstResult) {
        return Domain::Result<void>::failure(std::move(firstResult).error());
    }
    first.emplace(std::move(firstResult).value());
    auto secondResult = secondLease->acquireMigrationLock(context);
    if (!secondResult) {
        return Domain::Result<void>::failure(std::move(secondResult).error());
    }
    second.emplace(std::move(secondResult).value());
    return Domain::Result<void>::success();
}

} // namespace

Domain::Result<bool> DatabaseNamespaceLease::hasTransientLeaves() const noexcept
{
    for (const DatabaseLeafRole role :
         {DatabaseLeafRole::Wal, DatabaseLeafRole::SharedMemory, DatabaseLeafRole::Journal}) {
        auto exists = accessLeaf(role, DatabaseLeafAccess::Exists);
        if (!exists) {
            return Domain::Result<bool>::failure(std::move(exists).error());
        }
        if (exists.value()) {
            return Domain::Result<bool>::success(true);
        }
    }
    return Domain::Result<bool>::success(false);
}

Domain::Result<void> DatabaseNamespaceLease::publishClosedMainTo(
    DatabaseNamespaceLease& destination,
    const Domain::OperationContext& context) noexcept
{
    return publishClosedMainToImpl(destination, nullptr, nullptr, context);
}

Domain::Result<void> DatabaseNamespaceLease::publishClosedMainToWithSourceLock(
    DatabaseNamespaceLease& destination,
    const DatabaseMigrationLock& sourceLock,
    const Domain::OperationContext& context) noexcept
{
    return publishClosedMainToImpl(destination, &sourceLock, nullptr, context);
}

Domain::Result<DatabaseLeafLease>
DatabaseNamespaceLease::publishClosedMainToWithSourceLockAndRetain(
    DatabaseNamespaceLease& destination,
    const DatabaseMigrationLock& sourceLock,
    const Domain::OperationContext& context) noexcept
{
    DatabaseLeafLease retained;
    auto published = publishClosedMainToImpl(
        destination, &sourceLock, &retained, context);
    if (!published) {
        return Domain::Result<DatabaseLeafLease>::failure(
            std::move(published).error());
    }
    if (!retained || retained.role() != DatabaseLeafRole::Main) {
        return Domain::Result<DatabaseLeafLease>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "Database publication did not retain its exact published leaf."));
    }
    return Domain::Result<DatabaseLeafLease>::success(std::move(retained));
}

Domain::Result<void> DatabaseNamespaceLease::publishClosedMainToImpl(
    DatabaseNamespaceLease& destination,
    const DatabaseMigrationLock* const sourceLock,
    DatabaseLeafLease* const retainedPublishedLeaf,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (retainedPublishedLeaf != nullptr) {
            *retainedPublishedLeaf = DatabaseLeafLease{};
        }
        if (this == &destination ||
            equalInsensitive(canonicalMainDatabasePath(), destination.canonicalMainDatabasePath())) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "A staged database and its publication destination must be distinct leaves."));
        }
        auto validContext = checkOperation(context, "Closed database publication");
        if (!validContext) {
            return validContext;
        }
        auto sourceValid = revalidate();
        if (!sourceValid) {
            return sourceValid;
        }
        auto destinationValid = destination.revalidate();
        if (!destinationValid) {
            return destinationValid;
        }
        if (!equalInsensitive(canonicalDirectory(), destination.canonicalDirectory()) ||
            state_->directoryIdentity != destination.state_->directoryIdentity) {
            return Domain::Result<void>::failure(namespaceError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Atomic backup publication requires staged and destination leaves in the same anchored directory."));
        }
        auto closed = ensureClosedNamespaces(*this, destination);
        if (!closed) {
            return closed;
        }

        std::optional<DatabaseMigrationLock> firstLock;
        std::optional<DatabaseMigrationLock> secondLock;
        std::optional<DatabaseMigrationLock> destinationLock;
        DatabaseNamespaceLease* firstLockNamespace{};
        DatabaseNamespaceLease* secondLockNamespace{};

        const auto cleanupCreatedLock = [](
            DatabaseNamespaceLease& owner,
            std::optional<DatabaseMigrationLock>& heldLock,
            const bool removeExisting) noexcept
            -> Domain::Result<void> {
            if (!heldLock.has_value()) {
                return Domain::Result<void>::success();
            }
            const bool remove = removeExisting || heldLock->pinnedLeaf_.wasCreated();
            const DatabaseFileIdentity identity = heldLock->pinnedLeaf_.identity();
            // Remove the pathname while byte-lock ownership is still exclusive.
            // Waiters share deletion and will validate/retry after they are admitted
            // on this now-unlinked predecessor.
            auto removed = remove
                               ? owner.deleteLeaf(
                                     DatabaseLeafRole::MigrationLock, &identity, true)
                               : Domain::Result<void>::success();
            heldLock.reset();
            return removed;
        };
        const auto cleanupPublicationLocks = [&]() noexcept -> Domain::Result<void> {
            Domain::Result<void> firstCleanup = Domain::Result<void>::success();
            Domain::Result<void> secondCleanup = Domain::Result<void>::success();
            Domain::Result<void> destinationCleanup = Domain::Result<void>::success();
            if (secondLockNamespace != nullptr) {
                secondCleanup = cleanupCreatedLock(*secondLockNamespace, secondLock, false);
            }
            if (firstLockNamespace != nullptr) {
                firstCleanup = cleanupCreatedLock(*firstLockNamespace, firstLock, false);
            }
            destinationCleanup = cleanupCreatedLock(destination, destinationLock, true);
            if (!secondCleanup) {
                return secondCleanup;
            }
            if (!firstCleanup) {
                return firstCleanup;
            }
            return destinationCleanup;
        };

        if (sourceLock == nullptr) {
            const std::wstring& sourceLockPath =
                canonicalPath(DatabaseLeafRole::MigrationLock);
            const std::wstring& destinationLockPath =
                destination.canonicalPath(DatabaseLeafRole::MigrationLock);
            if (equalInsensitive(sourceLockPath, destinationLockPath) ||
                compareInsensitive(sourceLockPath, destinationLockPath) <= 0) {
                firstLockNamespace = this;
                if (!equalInsensitive(sourceLockPath, destinationLockPath)) {
                    secondLockNamespace = &destination;
                }
            } else {
                firstLockNamespace = &destination;
                secondLockNamespace = this;
            }
            auto locks = acquirePublicationLocks(
                *this, destination, context, firstLock, secondLock);
            if (!locks) {
                Domain::Error error = std::move(locks).error();
                auto lockCleanup = cleanupPublicationLocks();
                if (!lockCleanup) {
                    error.message += " Exact publication-lock cleanup also failed: ";
                    error.message += lockCleanup.error().message;
                }
                return Domain::Result<void>::failure(std::move(error));
            }
        } else {
            if (!sourceLock->locked_ || !sourceLock->pinnedLeaf_ ||
                sourceLock->pinnedLeaf_.role() != DatabaseLeafRole::MigrationLock) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Closed database publication requires the caller's held source lock."));
            }
            auto currentSourceLock = openLeaf(
                DatabaseLeafRole::MigrationLock,
                DatabaseLeafDisposition::OpenExisting,
                FILE_READ_ATTRIBUTES);
            if (!currentSourceLock) {
                return Domain::Result<void>::failure(
                    std::move(currentSourceLock).error());
            }
            if (currentSourceLock.value().identity() !=
                sourceLock->pinnedLeaf_.identity()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "The caller-held source publication lock changed identity.",
                    true));
            }
            if (!equalInsensitive(
                    canonicalPath(DatabaseLeafRole::MigrationLock),
                    destination.canonicalPath(DatabaseLeafRole::MigrationLock))) {
                auto acquiredDestination = destination.acquireMigrationLock(context);
                if (!acquiredDestination) {
                    return Domain::Result<void>::failure(
                        std::move(acquiredDestination).error());
                }
                destinationLock.emplace(std::move(acquiredDestination).value());
            }
        }

        std::optional<StableDatabaseLeaf> source;
        DatabaseFileIdentity sourceIdentity{};
        bool nativeRenameCompleted{};
        auto operation = [&]() -> Domain::Result<void> {
            closed = ensureClosedNamespaces(*this, destination);
            if (!closed) {
                return closed;
            }

            auto sourceTransient = hasTransientLeaves();
            if (!sourceTransient) {
                return Domain::Result<void>::failure(
                    std::move(sourceTransient).error());
            }
            auto destinationTransient = destination.hasTransientLeaves();
            if (!destinationTransient) {
                return Domain::Result<void>::failure(
                    std::move(destinationTransient).error());
            }
            if (sourceTransient.value() || destinationTransient.value()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "Closed backup publication refuses a namespace with WAL, shared-memory, or rollback-journal leaves.",
                    true));
            }

            auto openedSource = openStableLeaf(
                *state_, DatabaseLeafRole::Main, false, true);
            if (!openedSource) {
                return Domain::Result<void>::failure(
                    std::move(openedSource).error());
            }
            source.emplace(std::move(openedSource).value().value());
            auto existingDestination = openStableLeaf(
                *destination.state_, DatabaseLeafRole::Main, true, false);
            if (!existingDestination) {
                return Domain::Result<void>::failure(
                    std::move(existingDestination).error());
            }
            if (existingDestination.value().has_value()) {
                return Domain::Result<void>::failure(namespaceError(
                    Domain::ErrorCodes::Conflict,
                    "Closed database publication refuses to overwrite an existing destination.",
                    true));
            }
            validContext = checkOperation(context, "Closed database publication");
            if (!validContext) {
                return validContext;
            }
            sourceIdentity = source->identity;
            return renameStableLeaf(
                *source, canonicalMainDatabasePath(), *destination.state_,
                DatabaseLeafRole::Main, context, &nativeRenameCompleted);
        }();

        if (nativeRenameCompleted) {
            transferCohortIdentity(
                *state_, DatabaseLeafRole::Main,
                *destination.state_, DatabaseLeafRole::Main,
                sourceIdentity);
        }

        auto lockCleanup = cleanupPublicationLocks();
        if (!operation || !lockCleanup) {
            Domain::Error error = !operation
                ? std::move(operation).error()
                : std::move(lockCleanup).error();
            if (!operation && !lockCleanup) {
                error.message += " Exact publication-lock cleanup also failed: ";
                error.message += lockCleanup.error().message;
            }
            if (!operation && nativeRenameCompleted) {
                source.reset();
                auto removed = destination.deleteLeaf(
                    DatabaseLeafRole::Main, &sourceIdentity, false);
                if (!removed) {
                    error.message +=
                        " Exact cleanup of the owned unverified publication also failed: ";
                    error.message += removed.error().message;
                }
            }
            return Domain::Result<void>::failure(std::move(error));
        }

        if (retainedPublishedLeaf != nullptr) {
            *retainedPublishedLeaf = DatabaseLeafLease{
                DatabaseLeafRole::Main,
                std::move(source->handle),
                sourceIdentity,
                false,
                destination.state_};
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A closed database could not be atomically published."));
    }
}

Domain::Result<bool> DatabaseNamespaceLease::cleanupClosedStageWithLock(
    DatabaseMigrationLock& stageLock,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = checkOperation(context, "Locked database-stage cleanup");
        if (!validContext) {
            return Domain::Result<bool>::failure(std::move(validContext).error());
        }
        auto validStage = revalidate();
        if (!validStage) {
            return Domain::Result<bool>::failure(std::move(validStage).error());
        }
        if (openVfsFileCount() != 0U) {
            return Domain::Result<bool>::failure(namespaceError(
                Domain::ErrorCodes::DatabaseBusy,
                "A locked database stage still has open VFS files.",
                true));
        }
        if (!stageLock.locked_ || !stageLock.pinnedLeaf_ ||
            stageLock.pinnedLeaf_.role() != DatabaseLeafRole::MigrationLock) {
            return Domain::Result<bool>::failure(namespaceError(
                Domain::ErrorCodes::InvalidRequest,
                "Locked stage cleanup requires the caller's held exact stage lock."));
        }
        auto currentLock = openLeaf(
            DatabaseLeafRole::MigrationLock,
            DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES);
        if (!currentLock) {
            return Domain::Result<bool>::failure(std::move(currentLock).error());
        }
        if (currentLock.value().identity() != stageLock.pinnedLeaf_.identity()) {
            return Domain::Result<bool>::failure(namespaceError(
                Domain::ErrorCodes::Conflict,
                "The caller-held stage cleanup lock changed identity.",
                true));
        }

        const DatabaseFileIdentity lockIdentity = stageLock.pinnedLeaf_.identity();
        const bool lockExistedBeforeCleanup = !stageLock.pinnedLeaf_.wasCreated();
        const auto releaseAndDeleteLock = [&]() noexcept {
            auto removed = deleteLeaf(
                DatabaseLeafRole::MigrationLock, &lockIdentity, true);
            stageLock.release();
            return removed;
        };
        const auto failAfterLockCleanup =
            [&](Domain::Error error) -> Domain::Result<bool> {
            auto lockCleanup = releaseAndDeleteLock();
            if (!lockCleanup) {
                error.message +=
                    " The exact stage migration-lock leaf also could not be removed: ";
                error.message += lockCleanup.error().message;
            }
            return Domain::Result<bool>::failure(std::move(error));
        };

        bool removedAny{};
        constexpr std::array<DatabaseLeafRole, 4U> CleanupOrder{
            DatabaseLeafRole::Journal,
            DatabaseLeafRole::Wal,
            DatabaseLeafRole::SharedMemory,
            DatabaseLeafRole::Main};
        for (const DatabaseLeafRole role : CleanupOrder) {
            validContext = checkOperation(context, "Locked database-stage cleanup");
            if (!validContext) {
                return failAfterLockCleanup(std::move(validContext).error());
            }
            std::optional<DatabaseFileIdentity> identity;
            {
                auto leaf = openLeaf(
                    role,
                    DatabaseLeafDisposition::OpenExisting,
                    FILE_READ_ATTRIBUTES);
                if (!leaf) {
                    if (leaf.error().code == Domain::ErrorCodes::RecordNotFound) {
                        continue;
                    }
                    return failAfterLockCleanup(std::move(leaf).error());
                }
                identity = leaf.value().identity();
            }
            auto removed = deleteClosedLeaf(role, identity.value());
            if (!removed) {
                return failAfterLockCleanup(std::move(removed).error());
            }
            removedAny = true;
        }
        auto lockCleanup = releaseAndDeleteLock();
        if (!lockCleanup) {
            return Domain::Result<bool>::failure(std::move(lockCleanup).error());
        }
        return Domain::Result<bool>::success(
            removedAny || lockExistedBeforeCleanup);
    } catch (...) {
        return Domain::Result<bool>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "A caller-owned database-stage cleanup failed."));
    }
}

Domain::Result<std::size_t> DatabaseNamespaceLease::cleanupClosedStages(
    const std::span<const std::shared_ptr<DatabaseNamespaceLease>> staleStages,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (staleStages.size() > MaximumStaleStageLeasesPerCleanup) {
            return Domain::Result<std::size_t>::failure(namespaceError(
                Domain::ErrorCodes::LimitExceeded,
                "A stale database-stage cleanup may inspect at most 32 exact namespace leases."));
        }
        std::size_t cleanedStages{};
        for (const auto& stage : staleStages) {
            auto validContext = checkOperation(context, "Stale database-stage cleanup");
            if (!validContext) {
                return Domain::Result<std::size_t>::failure(std::move(validContext).error());
            }
            if (!stage) {
                return Domain::Result<std::size_t>::failure(namespaceError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Stale database-stage cleanup requires exact validated namespace lease objects."));
            }
            auto validStage = stage->revalidate();
            if (!validStage) {
                return Domain::Result<std::size_t>::failure(std::move(validStage).error());
            }
            if (stage->openVfsFileCount() != 0U) {
                return Domain::Result<std::size_t>::failure(namespaceError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "A stale database stage still has open VFS files.",
                    true));
            }
            auto lockResult = stage->acquireMigrationLock(context);
            if (!lockResult) {
                return Domain::Result<std::size_t>::failure(std::move(lockResult).error());
            }
            DatabaseMigrationLock stageLock = std::move(lockResult).value();
            auto cleaned = stage->cleanupClosedStageWithLock(stageLock, context);
            if (!cleaned) {
                return Domain::Result<std::size_t>::failure(
                    std::move(cleaned).error());
            }
            if (cleaned.value()) {
                ++cleanedStages;
            }
        }
        return Domain::Result<std::size_t>::success(cleanedStages);
    } catch (...) {
        return Domain::Result<std::size_t>::failure(namespaceError(
            Domain::ErrorCodes::InternalFailure,
            "Stale database-stage cleanup failed."));
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
