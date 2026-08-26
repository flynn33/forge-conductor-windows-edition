#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryArtifactStore.h"

#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/RelativeFileOperations.h"
#include "Infrastructure/Windows/Detail/BoundedSerialExecutor.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
namespace {

namespace WindowsDetail = Infrastructure::Windows::Detail;

constexpr std::size_t MaximumArtifactBytes = 32U * 1024U * 1024U;
constexpr std::size_t MaximumIoChunkBytes = 1024U * 1024U;
constexpr std::size_t MaximumPublishAttempts = 32U;
constexpr std::size_t ArtifactAdmissionStripeCount = 16U;
constexpr std::size_t MaximumRetainedOversizedArtifactsPerStripe = 16U;
constexpr std::size_t MaximumDirectoryEntriesPerScan = 1024U;
constexpr std::size_t ArtifactDirectoryBufferBytes = 16U * 1024U;
constexpr ULONG NativeFileIdExtdDirectoryInformation = 60U;
constexpr LONG NativeStatusNoMoreFiles = static_cast<LONG>(0x80000006UL);
constexpr std::wstring_view ExportsDirectoryName = L"exports";
constexpr std::wstring_view QuarantineDirectoryName = L"quarantine";
constexpr std::wstring_view ExportPrefix = L"memory-export-";
constexpr std::wstring_view QuarantinePrefix = L"corrupt-";
constexpr std::wstring_view ExportSuffix = L".json";

struct DirectoryIoStatusBlock final {
    union {
        LONG status;
        void* pointer;
    } result{};
    ULONG_PTR information{};
};

using NtQueryDirectoryFileFunction = LONG(NTAPI*)(
    HANDLE,
    HANDLE,
    void*,
    void*,
    DirectoryIoStatusBlock*,
    void*,
    ULONG,
    ULONG,
    BOOLEAN,
    void*,
    BOOLEAN);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(LONG);

[[nodiscard]] bool equalPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size() ||
        left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return ::CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool startsWithOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept
{
    return value.size() >= prefix.size() &&
           prefix.size() <=
               static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
           ::CompareStringOrdinal(
               value.data(),
               static_cast<int>(prefix.size()),
               prefix.data(),
               static_cast<int>(prefix.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool endsWithOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view suffix) noexcept
{
    return value.size() >= suffix.size() &&
           suffix.size() <=
               static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
           ::CompareStringOrdinal(
               value.data() + static_cast<std::ptrdiff_t>(
                                  value.size() - suffix.size()),
               static_cast<int>(suffix.size()),
               suffix.data(),
               static_cast<int>(suffix.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool startsWithPath(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept
{
    return value.size() > prefix.size() &&
           prefix.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
           ::CompareStringOrdinal(
               value.data(),
               static_cast<int>(prefix.size()),
               prefix.data(),
               static_cast<int>(prefix.size()),
               TRUE) == CSTR_EQUAL &&
           value[prefix.size()] == L'\\';
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
    } else if (value.starts_with(L"\\\\?\\")) {
        value.erase(0U, 4U);
    }
    return value;
}

[[nodiscard]] Domain::Result<std::wstring> finalPath(const HANDLE handle) noexcept
{
    try {
        const DWORD required = ::GetFinalPathNameByHandleW(
            handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U || required > Domain::PathText::MaximumBytes + 4U) {
            return Domain::Result<std::wstring>::failure(
                WindowsDetail::makeWin32Error(
                    "resolve a project-memory artifact handle", ::GetLastError()));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<std::wstring>::failure(
                WindowsDetail::makeWin32Error(
                    "resolve a project-memory artifact handle", ::GetLastError()));
        }
        return Domain::Result<std::wstring>::success(withoutExtendedPrefix(
            std::wstring{buffer.data(), static_cast<std::size_t>(written)}));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory artifact handle path could not be retained."));
    }
}

[[nodiscard]] Domain::Result<void> validateHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath,
    const bool directory) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
            "inspect a project-memory artifact object", ::GetLastError()));
    }
    const bool isDirectory =
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        isDirectory != directory) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact path is a reparse point or has the wrong type."));
    }
    if (directory) {
        FILE_CASE_SENSITIVE_INFO casePolicy{};
        if (::GetFileInformationByHandleEx(
                handle,
                FileCaseSensitiveInfo,
                &casePolicy,
                sizeof(casePolicy)) == FALSE) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "inspect project-memory artifact case policy", ::GetLastError()));
        }
        auto supported = WindowsDetail::WindowsPathResolver::
            validateDirectoryCaseSensitivityFlags(casePolicy.Flags);
        if (!supported) {
            return supported;
        }
    } else {
        BY_HANDLE_FILE_INFORMATION identity{};
        if (::GetFileInformationByHandle(handle, &identity) == FALSE) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "inspect project-memory artifact identity", ::GetLastError()));
        }
        if (identity.nNumberOfLinks != 1U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A project-memory artifact must not be a hard link."));
        }
    }
    auto openedPath = finalPath(handle);
    if (!openedPath) {
        return Domain::Result<void>::failure(std::move(openedPath).error());
    }
    if (!equalPath(openedPath.value(), expectedPath)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "A retained project-memory artifact object escaped its expected path."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateOnlyDefaultDataStream(
    const std::wstring_view path) noexcept
{
    try {
        WIN32_FIND_STREAM_DATA data{};
        const std::wstring nativePath = extendedPath(path);
        const HANDLE raw = ::FindFirstStreamW(
            nativePath.c_str(), FindStreamInfoStandard, &data, 0U);
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return Domain::Result<void>::success();
            }
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "enumerate project-memory artifact streams",
                error,
                Domain::ErrorCodes::IntegrityFailure));
        }
        struct FindHandleGuard final {
            HANDLE handle;
            ~FindHandleGuard() noexcept
            {
                if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                    static_cast<void>(::FindClose(handle));
                }
            }
        } finder{raw};
        if (std::wstring_view{data.cStreamName} != L"::$DATA") {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A project-memory artifact contains a non-default data stream."));
        }
        if (::FindNextStreamW(finder.handle, &data) != FALSE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A project-memory artifact contains an alternate data stream."));
        }
        const DWORD finalError = ::GetLastError();
        if (finalError != ERROR_HANDLE_EOF) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "enumerate project-memory artifact streams",
                finalError,
                Domain::ErrorCodes::IntegrityFailure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-memory artifact streams could not be validated."));
    }
}

struct ExportsDirectory final {
    WindowsDetail::UniqueHandle handle;
    std::wstring canonicalPath;
};

struct ArtifactCandidate final {
    std::wstring canonicalPath;
    std::wstring leaf;
};

[[nodiscard]] Domain::Result<ArtifactCandidate> resolveArtifactCandidate(
    const Domain::PathText& artifact,
    const std::wstring_view exportsPath) noexcept
{
    try {
        auto candidate = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
            artifact.value());
        if (!candidate) {
            return Domain::Result<ArtifactCandidate>::failure(
                std::move(candidate).error());
        }
        if (!startsWithPath(candidate.value(), exportsPath)) {
            return Domain::Result<ArtifactCandidate>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The import artifact is outside the target project's exports directory."));
        }
        const std::size_t separator = candidate.value().find_last_of(L'\\');
        if (separator == std::wstring::npos ||
            separator + 1U >= candidate.value().size() ||
            !equalPath(candidate.value().substr(0U, separator), exportsPath)) {
            return Domain::Result<ArtifactCandidate>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The import artifact is not an immediate project export child."));
        }
        return Domain::Result<ArtifactCandidate>::success(ArtifactCandidate{
            candidate.value(), candidate.value().substr(separator + 1U)});
    } catch (...) {
        return Domain::Result<ArtifactCandidate>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory artifact path could not be resolved safely."));
    }
}

[[nodiscard]] Domain::Result<WindowsDetail::UniqueHandle> openRootDirectory(
    const std::wstring_view canonicalPath,
    const bool allowChildCreation) noexcept
{
    try {
        const std::wstring nativePath = extendedPath(canonicalPath);
        WindowsDetail::UniqueHandle handle{::CreateFileW(
            nativePath.c_str(),
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
                (allowChildCreation ? FILE_ADD_SUBDIRECTORY : 0U),
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!handle) {
            return Domain::Result<WindowsDetail::UniqueHandle>::failure(
                WindowsDetail::makeWin32Error(
                    "retain the project-memory artifact root", ::GetLastError()));
        }
        auto validated = validateHandle(handle.get(), canonicalPath, true);
        if (!validated) {
            return Domain::Result<WindowsDetail::UniqueHandle>::failure(
                std::move(validated).error());
        }
        return Domain::Result<WindowsDetail::UniqueHandle>::success(std::move(handle));
    } catch (...) {
        return Domain::Result<WindowsDetail::UniqueHandle>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory artifact root could not be retained."));
    }
}

[[nodiscard]] Domain::Result<ExportsDirectory> openExportsDirectory(
    Contracts::IApplicationPaths& applicationPaths,
    const Domain::ProjectId& projectId,
    const bool create,
    const bool allowChildCreation,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto rootText = applicationPaths.projectRoot(projectId, context);
        if (!rootText) {
            return Domain::Result<ExportsDirectory>::failure(
                std::move(rootText).error());
        }
        auto root = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
            rootText.value().value());
        if (!root) {
            return Domain::Result<ExportsDirectory>::failure(std::move(root).error());
        }
        auto rootHandle = openRootDirectory(root.value(), create);
        if (!rootHandle) {
            return Domain::Result<ExportsDirectory>::failure(
                std::move(rootHandle).error());
        }
        std::wstring exportsPath = root.value();
        if (exportsPath.empty() || exportsPath.back() == L'\\' ||
            exportsPath.size() > Domain::PathText::MaximumBytes -
                                     ExportsDirectoryName.size() - 1U) {
            return Domain::Result<ExportsDirectory>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project-memory exports path exceeds its supported bound."));
        }
        exportsPath.push_back(L'\\');
        exportsPath.append(ExportsDirectoryName);
        DWORD desiredAccess =
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
        if (create) {
            desiredAccess |= FILE_ADD_FILE | FILE_DELETE_CHILD;
        }
        if (allowChildCreation) {
            desiredAccess |= FILE_ADD_SUBDIRECTORY;
        }
        WindowsDetail::RelativeOpenOptions options{};
        options.desiredAccess = desiredAccess;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        options.disposition = create
            ? WindowsDetail::RelativeOpenDisposition::OpenOrCreate
            : WindowsDetail::RelativeOpenDisposition::OpenExisting;
        options.fileAttributes =
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        options.objectType = WindowsDetail::RelativeObjectType::Directory;
        if (create) {
            auto active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "create the project-memory exports directory");
            if (!active) {
                return Domain::Result<ExportsDirectory>::failure(
                    std::move(active).error());
            }
        }
        auto exportsHandle = WindowsDetail::openRelative(
            rootHandle.value().get(), ExportsDirectoryName, options);
        if (!exportsHandle) {
            return Domain::Result<ExportsDirectory>::failure(
                WindowsDetail::makeWin32Error(
                    create ? "create or retain the project-memory exports directory"
                           : "retain the project-memory exports directory",
                    exportsHandle.win32Error,
                    create ? Domain::ErrorCodes::InternalFailure
                           : Domain::ErrorCodes::InvalidRequest));
        }
        auto validated = validateHandle(
            exportsHandle.handle.get(), exportsPath, true);
        if (!validated) {
            return Domain::Result<ExportsDirectory>::failure(
                std::move(validated).error());
        }
        return Domain::Result<ExportsDirectory>::success(ExportsDirectory{
            std::move(exportsHandle.handle), std::move(exportsPath)});
    } catch (...) {
        return Domain::Result<ExportsDirectory>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory exports directory could not be retained."));
    }
}

[[nodiscard]] Domain::Result<ExportsDirectory> openQuarantineDirectory(
    const ExportsDirectory& exports,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "create the project-memory artifact quarantine");
        if (!active) {
            return Domain::Result<ExportsDirectory>::failure(
                std::move(active).error());
        }
        if (exports.canonicalPath.empty() ||
            exports.canonicalPath.back() == L'\\' ||
            exports.canonicalPath.size() > Domain::PathText::MaximumBytes -
                                                QuarantineDirectoryName.size() - 1U) {
            return Domain::Result<ExportsDirectory>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project-memory quarantine path exceeds its supported bound."));
        }
        std::wstring quarantinePath = exports.canonicalPath;
        quarantinePath.push_back(L'\\');
        quarantinePath.append(QuarantineDirectoryName);

        WindowsDetail::RelativeOpenOptions options{};
        options.desiredAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                                FILE_READ_ATTRIBUTES | FILE_ADD_FILE;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        options.disposition = WindowsDetail::RelativeOpenDisposition::OpenOrCreate;
        options.fileAttributes =
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        options.objectType = WindowsDetail::RelativeObjectType::Directory;
        auto opened = WindowsDetail::openRelative(
            exports.handle.get(), QuarantineDirectoryName, options);
        if (!opened) {
            return Domain::Result<ExportsDirectory>::failure(
                WindowsDetail::makeWin32Error(
                    "create or retain the project-memory artifact quarantine",
                    opened.win32Error,
                    Domain::ErrorCodes::IntegrityFailure));
        }
        auto validated = validateHandle(
            opened.handle.get(), quarantinePath, true);
        if (!validated) {
            return Domain::Result<ExportsDirectory>::failure(
                std::move(validated).error());
        }
        return Domain::Result<ExportsDirectory>::success(ExportsDirectory{
            std::move(opened.handle), std::move(quarantinePath)});
    } catch (...) {
        return Domain::Result<ExportsDirectory>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory artifact quarantine could not be retained."));
    }
}

enum class ArtifactDirectoryKind : unsigned char {
    Exports,
    Quarantine,
};

struct ArtifactDirectoryInventory final {
    std::size_t ownedFileCount{};
    bool quarantineDirectoryPresent{};
};

[[nodiscard]] bool hasArtifactNameShape(
    const std::wstring_view name,
    const std::wstring_view prefix,
    const std::wstring_view suffix) noexcept
{
    return name.size() > prefix.size() + suffix.size() &&
           startsWithOrdinalIgnoreCase(name, prefix) &&
           endsWithOrdinalIgnoreCase(name, suffix);
}

[[nodiscard]] bool isOwnedArtifactName(
    const std::wstring_view name,
    const ArtifactDirectoryKind directoryKind) noexcept
{
    if (directoryKind == ArtifactDirectoryKind::Quarantine) {
        return hasArtifactNameShape(name, QuarantinePrefix, ExportSuffix);
    }
    constexpr std::wstring_view StagingPrefix = L".memory-export-";
    constexpr std::wstring_view StagingSuffix = L".json.tmp";
    return hasArtifactNameShape(name, ExportPrefix, ExportSuffix) ||
           hasArtifactNameShape(name, StagingPrefix, StagingSuffix);
}

[[nodiscard]] Domain::Result<ArtifactDirectoryInventory>
scanArtifactDirectory(
    const HANDLE directory,
    const ArtifactDirectoryKind directoryKind,
    const Domain::OperationContext& context) noexcept
{
    try {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntQueryDirectoryFile = ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtQueryDirectoryFileFunction>(
                  ::GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        const auto rtlNtStatusToDosError = ntdll == nullptr
            ? nullptr
            : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                  ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntQueryDirectoryFile == nullptr || rtlNtStatusToDosError == nullptr) {
            return Domain::Result<ArtifactDirectoryInventory>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "Handle-relative project-memory artifact enumeration is unavailable."));
        }

        ArtifactDirectoryInventory inventory{};
        std::array<std::uint64_t,
                   ArtifactDirectoryBufferBytes / sizeof(std::uint64_t)> buffer{};
        bool firstQuery{true};
        std::size_t entriesObserved{};
        for (std::size_t queryCount = 0U;
             queryCount < MaximumDirectoryEntriesPerScan + 2U;
             ++queryCount) {
            auto active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "count project-memory artifact files");
            if (!active) {
                return Domain::Result<ArtifactDirectoryInventory>::failure(
                    std::move(active).error());
            }
            DirectoryIoStatusBlock ioStatus{};
            const LONG status = ntQueryDirectoryFile(
                directory,
                nullptr,
                nullptr,
                nullptr,
                &ioStatus,
                buffer.data(),
                static_cast<ULONG>(ArtifactDirectoryBufferBytes),
                NativeFileIdExtdDirectoryInformation,
                FALSE,
                nullptr,
                firstQuery ? TRUE : FALSE);
            firstQuery = false;
            if (status == NativeStatusNoMoreFiles) {
                return Domain::Result<ArtifactDirectoryInventory>::success(
                    inventory);
            }
            if (status < 0) {
                const DWORD error = static_cast<DWORD>(
                    rtlNtStatusToDosError(status));
                if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
                    return Domain::Result<ArtifactDirectoryInventory>::success(
                        inventory);
                }
                return Domain::Result<ArtifactDirectoryInventory>::failure(
                    WindowsDetail::makeWin32Error(
                        "enumerate project-memory artifact files",
                        error,
                        Domain::ErrorCodes::IntegrityFailure));
            }
            if (ioStatus.information == 0U) {
                return Domain::Result<ArtifactDirectoryInventory>::success(
                    inventory);
            }
            if (ioStatus.information > ArtifactDirectoryBufferBytes) {
                return Domain::Result<ArtifactDirectoryInventory>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Project-memory artifact enumeration exceeded its native buffer."));
            }

            std::size_t offset{};
            for (;;) {
                active = WindowsDetail::validateOperationContext(
                    context,
                    std::chrono::steady_clock::now(),
                    "count a project-memory artifact file");
                if (!active) {
                    return Domain::Result<ArtifactDirectoryInventory>::failure(
                        std::move(active).error());
                }
                if (offset + offsetof(FILE_ID_EXTD_DIR_INFO, FileName) >
                    ioStatus.information) {
                    return Domain::Result<ArtifactDirectoryInventory>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "Project-memory artifact enumeration returned an invalid entry."));
                }
                const auto* const information =
                    reinterpret_cast<const FILE_ID_EXTD_DIR_INFO*>(
                        reinterpret_cast<const std::byte*>(buffer.data()) + offset);
                if ((information->FileNameLength % sizeof(wchar_t)) != 0U ||
                    information->FileNameLength >
                        ioStatus.information - offset -
                            offsetof(FILE_ID_EXTD_DIR_INFO, FileName)) {
                    return Domain::Result<ArtifactDirectoryInventory>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "Project-memory artifact enumeration returned an invalid name."));
                }
                ++entriesObserved;
                if (entriesObserved > MaximumDirectoryEntriesPerScan) {
                    return Domain::Result<ArtifactDirectoryInventory>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "Project-memory artifact enumeration exceeded its bounded scan."));
                }
                const std::wstring_view name{
                    information->FileName,
                    static_cast<std::size_t>(information->FileNameLength) /
                        sizeof(wchar_t)};
                const bool isDirectory =
                    (information->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
                const bool isReparse =
                    (information->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
                if (directoryKind == ArtifactDirectoryKind::Exports &&
                    equalPath(name, QuarantineDirectoryName)) {
                    if (!isDirectory || isReparse ||
                        inventory.quarantineDirectoryPresent) {
                        return Domain::Result<ArtifactDirectoryInventory>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::IntegrityFailure,
                                "The project-memory quarantine entry has an invalid type."));
                    }
                    inventory.quarantineDirectoryPresent = true;
                } else if (isOwnedArtifactName(name, directoryKind)) {
                    if (isDirectory || isReparse) {
                        return Domain::Result<ArtifactDirectoryInventory>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::IntegrityFailure,
                                "A project-memory artifact inventory entry is not a regular file."));
                    }
                    ++inventory.ownedFileCount;
                    if (inventory.ownedFileCount >
                        Contracts::IProjectMemoryArtifactStore::
                            MaximumOwnedArtifactFilesPerProject) {
                        return Domain::Result<ArtifactDirectoryInventory>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::LimitExceeded,
                                "The project-memory artifact file quota was exceeded."));
                    }
                }

                if (information->NextEntryOffset == 0U) {
                    break;
                }
                if (information->NextEntryOffset <
                        offsetof(FILE_ID_EXTD_DIR_INFO, FileName) ||
                    information->NextEntryOffset > ioStatus.information - offset) {
                    return Domain::Result<ArtifactDirectoryInventory>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "Project-memory artifact enumeration returned an invalid chain."));
                }
                offset += information->NextEntryOffset;
            }
        }
        return Domain::Result<ArtifactDirectoryInventory>::failure(
            Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Project-memory artifact enumeration exceeded its bounded query count."));
    } catch (...) {
        return Domain::Result<ArtifactDirectoryInventory>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Project-memory artifact inventory failed safely."));
    }
}

[[nodiscard]] Domain::Result<std::size_t> countOwnedArtifactFiles(
    const ExportsDirectory& exports,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validated = validateHandle(
            exports.handle.get(), exports.canonicalPath, true);
        if (!validated) {
            return Domain::Result<std::size_t>::failure(
                std::move(validated).error());
        }
        auto exportInventory = scanArtifactDirectory(
            exports.handle.get(), ArtifactDirectoryKind::Exports, context);
        if (!exportInventory) {
            return Domain::Result<std::size_t>::failure(
                std::move(exportInventory).error());
        }
        std::size_t total = exportInventory.value().ownedFileCount;
        if (!exportInventory.value().quarantineDirectoryPresent) {
            return Domain::Result<std::size_t>::success(total);
        }

        if (exports.canonicalPath.size() > Domain::PathText::MaximumBytes -
                                               QuarantineDirectoryName.size() - 1U) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project-memory quarantine path exceeds its supported bound."));
        }
        std::wstring quarantinePath = exports.canonicalPath;
        quarantinePath.push_back(L'\\');
        quarantinePath.append(QuarantineDirectoryName);
        WindowsDetail::RelativeOpenOptions options{};
        options.desiredAccess =
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        options.disposition = WindowsDetail::RelativeOpenDisposition::OpenExisting;
        options.objectType = WindowsDetail::RelativeObjectType::Directory;
        auto quarantine = WindowsDetail::openRelative(
            exports.handle.get(), QuarantineDirectoryName, options);
        if (!quarantine) {
            return Domain::Result<std::size_t>::failure(
                WindowsDetail::makeWin32Error(
                    "retain the project-memory artifact quarantine for inventory",
                    quarantine.win32Error,
                    Domain::ErrorCodes::IntegrityFailure));
        }
        validated = validateHandle(
            quarantine.handle.get(), quarantinePath, true);
        if (!validated) {
            return Domain::Result<std::size_t>::failure(
                std::move(validated).error());
        }
        auto quarantineInventory = scanArtifactDirectory(
            quarantine.handle.get(), ArtifactDirectoryKind::Quarantine, context);
        if (!quarantineInventory) {
            return Domain::Result<std::size_t>::failure(
                std::move(quarantineInventory).error());
        }
        if (quarantineInventory.value().quarantineDirectoryPresent ||
            quarantineInventory.value().ownedFileCount >
                Contracts::IProjectMemoryArtifactStore::
                    MaximumOwnedArtifactFilesPerProject - total) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The project-memory artifact file quota was exceeded."));
        }
        total += quarantineInventory.value().ownedFileCount;
        return Domain::Result<std::size_t>::success(total);
    } catch (...) {
        return Domain::Result<std::size_t>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-memory artifact counting failed safely."));
    }
}

class PendingArtifactFile final {
public:
    explicit PendingArtifactFile(WindowsDetail::UniqueHandle handle) noexcept
        : handle_{std::move(handle)}
    {
    }

    PendingArtifactFile(const PendingArtifactFile&) = delete;
    PendingArtifactFile& operator=(const PendingArtifactFile&) = delete;
    PendingArtifactFile(PendingArtifactFile&&) = delete;
    PendingArtifactFile& operator=(PendingArtifactFile&&) = delete;

    ~PendingArtifactFile() noexcept
    {
        if (cleanupRequired_ && handle_) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            static_cast<void>(::SetFileInformationByHandle(
                handle_.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        }
    }

    [[nodiscard]] HANDLE handle() const noexcept { return handle_.get(); }

    [[nodiscard]] Domain::Result<void> discard() noexcept
    {
        if (!cleanupRequired_ || !handle_) {
            return Domain::Result<void>::success();
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (::SetFileInformationByHandle(
                handle_.get(),
                FileDispositionInfo,
                &disposition,
                sizeof(disposition)) == FALSE) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "discard an incomplete project-memory artifact",
                ::GetLastError()));
        }
        const HANDLE raw = handle_.get();
        if (::CloseHandle(raw) == FALSE) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "close an incomplete project-memory artifact",
                ::GetLastError()));
        }
        static_cast<void>(handle_.release());
        cleanupRequired_ = false;
        return Domain::Result<void>::success();
    }

    void commit() noexcept { cleanupRequired_ = false; }

private:
    WindowsDetail::UniqueHandle handle_;
    bool cleanupRequired_{true};
};

[[nodiscard]] Domain::Result<void> writeAll(
    const HANDLE handle,
    const std::span<const std::byte> content,
    const Domain::OperationContext& context) noexcept
{
    std::size_t offset{};
    while (offset < content.size()) {
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "continue writing a project-memory artifact");
        if (!active) {
            return active;
        }
        const auto remaining = content.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining,
            MaximumIoChunkBytes));
        DWORD written{};
        if (::WriteFile(
                handle,
                content.data() + static_cast<std::ptrdiff_t>(offset),
                requested,
                &written,
                nullptr) == FALSE ||
            written == 0U) {
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "write a project-memory artifact", ::GetLastError()));
        }
        offset += written;
    }
    auto active = WindowsDetail::validateOperationContext(
        context,
        std::chrono::steady_clock::now(),
        "flush a project-memory artifact");
    if (!active) {
        return active;
    }
    if (::FlushFileBuffers(handle) == FALSE) {
        return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
            "flush a project-memory artifact", ::GetLastError()));
    }
    return WindowsDetail::validateOperationContext(
        context,
        std::chrono::steady_clock::now(),
        "finish writing a project-memory artifact");
}

[[nodiscard]] Domain::Result<void> renameRelative(
    const HANDLE source,
    const HANDLE destinationDirectory,
    const std::wstring_view destinationName) noexcept
{
    try {
        struct NativeIoStatusBlock final {
            union {
                LONG status;
                void* pointer;
            } result{};
            ULONG_PTR information{};
        };
        using NtSetInformationFileFunction = LONG(NTAPI*)(
            HANDLE, NativeIoStatusBlock*, void*, ULONG, ULONG);
        using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(LONG);
        constexpr ULONG NativeFileRenameInformationEx = 65U;
        if (source == nullptr || source == INVALID_HANDLE_VALUE ||
            destinationDirectory == nullptr ||
            destinationDirectory == INVALID_HANDLE_VALUE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact rename handles are invalid."));
        }
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        if (destinationName.empty() ||
            destinationName.find_first_of(L"\\/:") != std::wstring_view::npos ||
            informationBytes > (std::numeric_limits<DWORD>::max)()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact filename exceeds its native bound."));
        }
        std::vector<std::uint64_t> storage(
            (informationBytes + sizeof(std::uint64_t) - 1U) /
            sizeof(std::uint64_t));
        auto* const information =
            reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        std::memset(information, 0, informationBytes);
        information->Flags = 0U;
        // Native FileRenameInformationEx interprets a simple leaf name with a
        // null root relative to the source file's retained parent directory.
        information->RootDirectory = destinationDirectory;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntSetInformationFile = ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtSetInformationFileFunction>(
                  ::GetProcAddress(ntdll, "NtSetInformationFile"));
        const auto rtlNtStatusToDosError = ntdll == nullptr
            ? nullptr
            : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                  ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "Native same-directory artifact rename is unavailable."));
        }
        NativeIoStatusBlock ioStatus{};
        const LONG status = ntSetInformationFile(
            source,
            &ioStatus,
            information,
            static_cast<ULONG>(informationBytes),
            NativeFileRenameInformationEx);
        if (status < 0) {
            const DWORD error = static_cast<DWORD>(
                rtlNtStatusToDosError(status));
            return Domain::Result<void>::failure(WindowsDetail::makeWin32Error(
                "publish a project-memory artifact",
                error,
                error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                    ? Domain::ErrorCodes::Conflict
                    : Domain::ErrorCodes::InternalFailure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory artifact could not be published atomically."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> moveToQuarantine(
    const HANDLE source,
    const ExportsDirectory& exports,
    const std::wstring_view sourceLeaf,
    Contracts::IUuidGenerator& uuidGenerator,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "create a quarantine for a corrupt project-memory artifact");
        if (!active) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(active).error());
        }
        auto ownedFileCount = countOwnedArtifactFiles(exports, context);
        if (!ownedFileCount) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(ownedFileCount).error());
        }
        const bool sourceAlreadyCounted = isOwnedArtifactName(
            sourceLeaf, ArtifactDirectoryKind::Exports);
        if (!sourceAlreadyCounted &&
            ownedFileCount.value() >=
                Contracts::IProjectMemoryArtifactStore::
                    MaximumOwnedArtifactFilesPerProject) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Quarantining this import would exceed the project-memory artifact file quota."));
        }
        auto quarantine = openQuarantineDirectory(exports, context);
        if (!quarantine) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(quarantine).error());
        }

        for (std::size_t attempt = 0U; attempt < MaximumPublishAttempts; ++attempt) {
            auto uuid = uuidGenerator.next();
            if (!uuid) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(uuid).error());
            }
            auto uuidText = WindowsDetail::strictUtf8ToUtf16(
                uuid.value().value());
            if (!uuidText) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(uuidText).error());
            }
            std::wstring quarantineName{QuarantinePrefix};
            quarantineName.append(uuidText.value());
            quarantineName.append(ExportSuffix);
            std::wstring quarantinePath = quarantine.value().canonicalPath;
            quarantinePath.push_back(L'\\');
            quarantinePath.append(quarantineName);
            auto quarantineText = WindowsDetail::WindowsPathResolver::toPathText(
                quarantinePath);
            if (!quarantineText) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(quarantineText).error());
            }

            active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "move a corrupt project-memory artifact into quarantine");
            if (!active) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(active).error());
            }
            auto exportsValidated = validateHandle(
                exports.handle.get(), exports.canonicalPath, true);
            if (!exportsValidated) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(exportsValidated).error());
            }
            auto quarantineValidated = validateHandle(
                quarantine.value().handle.get(),
                quarantine.value().canonicalPath,
                true);
            if (!quarantineValidated) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(quarantineValidated).error());
            }
            auto renamed = renameRelative(
                source, quarantine.value().handle.get(), quarantineName);
            if (!renamed) {
                if (renamed.error().code == Domain::ErrorCodes::Conflict) {
                    continue;
                }
                return Domain::Result<Domain::PathText>::failure(
                    std::move(renamed).error());
            }
            auto validated = validateHandle(source, quarantinePath, false);
            if (!validated) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(validated).error());
            }
            return Domain::Result<Domain::PathText>::success(
                std::move(quarantineText).value());
        }
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "A unique corrupt-artifact quarantine filename could not be allocated."));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The retained project-memory artifact could not be quarantined."));
    }
}

[[nodiscard]] Domain::Result<std::uint64_t> retainedArtifactSize(
    const HANDLE handle) noexcept
{
    FILE_STANDARD_INFO information{};
    if (::GetFileInformationByHandleEx(
            handle, FileStandardInfo, &information, sizeof(information)) == FALSE) {
        return Domain::Result<std::uint64_t>::failure(
            WindowsDetail::makeWin32Error(
                "inspect a project-memory artifact size", ::GetLastError()));
    }
    if (information.EndOfFile.QuadPart < 0) {
        return Domain::Result<std::uint64_t>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact reported a negative size."));
    }
    return Domain::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(information.EndOfFile.QuadPart));
}

[[nodiscard]] Domain::Result<std::vector<std::byte>> readAll(
    const HANDLE handle,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    auto measuredSize = retainedArtifactSize(handle);
    if (!measuredSize) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            measuredSize.error().code,
            measuredSize.error().message,
            measuredSize.error().retryable));
    }
    const auto size = measuredSize.value();
    if (size > maximumBytes || size > MaximumArtifactBytes) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "A project-memory artifact exceeds the 32 MiB limit."));
    }
    try {
        std::vector<std::byte> content(static_cast<std::size_t>(size));
        std::size_t offset{};
        while (offset < content.size()) {
            auto active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "continue reading a project-memory artifact");
            if (!active) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(active).error());
            }
            const DWORD requested = static_cast<DWORD>((std::min)(
                content.size() - offset,
                MaximumIoChunkBytes));
            DWORD received{};
            if (::ReadFile(
                    handle,
                    content.data() + static_cast<std::ptrdiff_t>(offset),
                    requested,
                    &received,
                    nullptr) == FALSE) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    WindowsDetail::makeWin32Error(
                        "read a project-memory artifact", ::GetLastError()));
            }
            if (received == 0U) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "A project-memory artifact ended before its retained size."));
            }
            offset += received;
        }
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "verify a project-memory artifact end");
        if (!active) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(active).error());
        }
        std::byte probe{};
        DWORD received{};
        if (::ReadFile(handle, &probe, 1U, &received, nullptr) == FALSE) {
            return Domain::Result<std::vector<std::byte>>::failure(
                WindowsDetail::makeWin32Error(
                    "verify a project-memory artifact end", ::GetLastError()));
        }
        if (received != 0U) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A project-memory artifact changed while it was retained."));
        }
        active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "finish reading a project-memory artifact");
        if (!active) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(active).error());
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(content));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded project-memory artifact buffer could not be allocated."));
    }
}

[[nodiscard]] Domain::Result<void> compareRetainedContent(
    const HANDLE handle,
    const std::span<const std::byte> expected,
    const Domain::OperationContext& context) noexcept
{
    auto measuredSize = retainedArtifactSize(handle);
    if (!measuredSize) {
        return Domain::Result<void>::failure(std::move(measuredSize).error());
    }
    if (measuredSize.value() != expected.size()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The corrupt project-memory artifact changed after its retained read."));
    }
    try {
        std::vector<std::byte> buffer(
            (std::min)(expected.size(), MaximumIoChunkBytes));
        std::size_t offset{};
        while (offset < expected.size()) {
            auto active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "compare a retained corrupt project-memory artifact");
            if (!active) {
                return active;
            }
            const DWORD requested = static_cast<DWORD>((std::min)(
                expected.size() - offset,
                buffer.size()));
            DWORD received{};
            if (::ReadFile(
                    handle,
                    buffer.data(),
                    requested,
                    &received,
                    nullptr) == FALSE) {
                return Domain::Result<void>::failure(
                    WindowsDetail::makeWin32Error(
                        "compare a retained corrupt project-memory artifact",
                        ::GetLastError()));
            }
            if (received != requested ||
                !std::equal(
                    buffer.cbegin(),
                    buffer.cbegin() + static_cast<std::ptrdiff_t>(received),
                    expected.begin() + static_cast<std::ptrdiff_t>(offset),
                    expected.begin() + static_cast<std::ptrdiff_t>(
                                           offset + received))) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The corrupt project-memory artifact changed after its retained read."));
            }
            offset += received;
        }
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "verify a retained corrupt project-memory artifact end");
        if (!active) {
            return active;
        }
        std::byte probe{};
        DWORD received{};
        if (::ReadFile(handle, &probe, 1U, &received, nullptr) == FALSE) {
            return Domain::Result<void>::failure(
                WindowsDetail::makeWin32Error(
                    "verify a retained corrupt project-memory artifact end",
                    ::GetLastError()));
        }
        if (received != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The corrupt project-memory artifact changed after its retained read."));
        }
        return WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "finish comparing a retained corrupt project-memory artifact");
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded corrupt-artifact comparison buffer could not be allocated."));
    }
}

struct RetainedOversizedArtifact final {
    std::string projectId;
    std::wstring canonicalPath;
    std::wstring sourceLeaf;
    std::size_t maximumBytes{};
    std::uint64_t observedBytes{};
    ExportsDirectory exports;
    WindowsDetail::UniqueHandle source;
};

} // namespace

struct WindowsProjectMemoryArtifactStore::Impl final {
    Impl(
        std::shared_ptr<Contracts::IApplicationPaths> configuredApplicationPaths,
        std::shared_ptr<Contracts::IUuidGenerator> configuredUuidGenerator)
        : applicationPaths{std::move(configuredApplicationPaths)},
          uuidGenerator{std::move(configuredUuidGenerator)}
    {
    }

    [[nodiscard]] WindowsDetail::BoundedSerialExecutor& admissionFor(
        const Domain::ProjectId& projectId) noexcept
    {
        return admissions[admissionIndex(projectId)];
    }

    [[nodiscard]] Domain::Result<void> retainOversized(
        const Domain::ProjectId& projectId,
        std::wstring canonicalPath,
        std::wstring sourceLeaf,
        const std::size_t maximumBytes,
        const std::uint64_t observedBytes,
        ExportsDirectory exports,
        WindowsDetail::UniqueHandle source) noexcept
    {
        try {
            auto& slots = retainedOversized[admissionIndex(projectId)];
            std::optional<RetainedOversizedArtifact>* available{};
            for (auto& slot : slots) {
                if (slot.has_value() &&
                    slot->projectId == projectId.value() &&
                    equalPath(slot->canonicalPath, canonicalPath)) {
                    slot.reset();
                    available = &slot;
                    break;
                }
                if (!slot.has_value() && available == nullptr) {
                    available = &slot;
                }
            }
            if (available == nullptr) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The bounded retained-artifact quarantine catalog is full."));
            }
            available->emplace(RetainedOversizedArtifact{
                projectId.value(),
                std::move(canonicalPath),
                std::move(sourceLeaf),
                maximumBytes,
                observedBytes,
                std::move(exports),
                std::move(source)});
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The oversized-artifact handle could not be retained safely."));
        }
    }

    void discardRetainedOversized(
        const Domain::ProjectId& projectId,
        const std::wstring_view canonicalPath) noexcept
    {
        auto& slots = retainedOversized[admissionIndex(projectId)];
        for (auto& slot : slots) {
            if (slot.has_value() &&
                slot->projectId == projectId.value() &&
                equalPath(slot->canonicalPath, canonicalPath)) {
                slot.reset();
            }
        }
    }

    [[nodiscard]] Domain::Result<RetainedOversizedArtifact>
    takeRetainedOversized(
        const Domain::ProjectId& projectId,
        const std::wstring_view canonicalPath,
        const std::size_t maximumBytes) noexcept
    {
        try {
            auto& slots = retainedOversized[admissionIndex(projectId)];
            for (auto& slot : slots) {
                if (!slot.has_value() ||
                    slot->projectId != projectId.value() ||
                    !equalPath(slot->canonicalPath, canonicalPath)) {
                    continue;
                }
                if (slot->maximumBytes != maximumBytes) {
                    return Domain::Result<RetainedOversizedArtifact>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The oversized-artifact quarantine bound changed after read."));
                }
                RetainedOversizedArtifact retained = std::move(slot).value();
                slot.reset();
                return Domain::Result<RetainedOversizedArtifact>::success(
                    std::move(retained));
            }
            return Domain::Result<RetainedOversizedArtifact>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "No matching retained oversized-artifact handle is available."));
        } catch (...) {
            return Domain::Result<RetainedOversizedArtifact>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The oversized-artifact handle could not be consumed safely."));
        }
    }

    [[nodiscard]] static std::size_t admissionIndex(
        const Domain::ProjectId& projectId) noexcept
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char character : projectId.value()) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(hash % ArtifactAdmissionStripeCount);
    }

    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator;
    std::array<WindowsDetail::BoundedSerialExecutor,
               ArtifactAdmissionStripeCount> admissions;
    std::array<
        std::array<
            std::optional<RetainedOversizedArtifact>,
            MaximumRetainedOversizedArtifactsPerStripe>,
        ArtifactAdmissionStripeCount> retainedOversized;
};

WindowsProjectMemoryArtifactStore::WindowsProjectMemoryArtifactStore(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator)
    : implementation_{std::make_unique<Impl>(
          std::move(applicationPaths), std::move(uuidGenerator))}
{
    if (!implementation_->applicationPaths || !implementation_->uuidGenerator) {
        throw std::invalid_argument(
            "The project-memory artifact store requires application paths and UUIDs.");
    }
}

WindowsProjectMemoryArtifactStore::~WindowsProjectMemoryArtifactStore() noexcept = default;

Domain::Result<Domain::PathText> WindowsProjectMemoryArtifactStore::publish(
    const Domain::ProjectId& projectId,
    const std::span<const std::byte> content,
    const Contracts::WorkspaceAuthority& writeAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "publish project memory");
        if (!active) {
            return Domain::Result<Domain::PathText>::failure(std::move(active).error());
        }
        if (writeAuthority.projectId() != projectId ||
            writeAuthority.intent() != Domain::FileAccess::Write ||
            !authorization.matches(writeAuthority, context) ||
            !authorization.matchesProject(projectId) ||
            authorization.toolName() != "project_memory.export" ||
            authorization.effect() != Domain::ToolEffect::Write) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The project-memory export authority or tool capability is mismatched."));
        }
        if (content.size() > MaximumArtifactBytes) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact exceeds the 32 MiB limit."));
        }
        auto admitted = implementation_->admissionFor(projectId).acquire(
            context, "Publish a project-memory artifact");
        if (!admitted) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);
        auto exports = openExportsDirectory(
            *implementation_->applicationPaths,
            projectId,
            true,
            false,
            context);
        if (!exports) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(exports).error());
        }
        auto currentFileCount = countOwnedArtifactFiles(exports.value(), context);
        if (!currentFileCount) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(currentFileCount).error());
        }
        if (currentFileCount.value() >=
            Contracts::IProjectMemoryArtifactStore::
                MaximumOwnedArtifactFilesPerProject) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The project-memory artifact file quota was reached."));
        }

        for (std::size_t attempt = 0U; attempt < MaximumPublishAttempts; ++attempt) {
            active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "allocate a project-memory artifact filename");
            if (!active) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(active).error());
            }
            auto uuid = implementation_->uuidGenerator->next();
            if (!uuid) {
                return Domain::Result<Domain::PathText>::failure(std::move(uuid).error());
            }
            const auto uuidText = WindowsDetail::strictUtf8ToUtf16(
                uuid.value().value());
            if (!uuidText) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(uuidText).error());
            }
            std::wstring finalName{ExportPrefix};
            finalName.append(uuidText.value());
            finalName.append(ExportSuffix);
            std::wstring temporaryName{L"."};
            temporaryName.append(finalName);
            temporaryName.append(L".tmp");

            active = WindowsDetail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "create a project-memory artifact staging file");
            if (!active) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(active).error());
            }

            WindowsDetail::RelativeOpenOptions options{};
            options.desiredAccess = GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE;
            options.shareAccess = 0U;
            options.disposition = WindowsDetail::RelativeOpenDisposition::CreateNew;
            options.objectType = WindowsDetail::RelativeObjectType::File;
            options.writeThrough = true;
            auto temporary = WindowsDetail::openRelative(
                exports.value().handle.get(), temporaryName, options);
            if (!temporary) {
                if (temporary.win32Error == ERROR_FILE_EXISTS ||
                    temporary.win32Error == ERROR_ALREADY_EXISTS) {
                    continue;
                }
                return Domain::Result<Domain::PathText>::failure(
                    WindowsDetail::makeWin32Error(
                        "create a project-memory artifact", temporary.win32Error));
            }
            PendingArtifactFile pending{std::move(temporary.handle)};
            auto attempted = [&]() -> Domain::Result<Domain::PathText> {
                auto stagedFileCount = countOwnedArtifactFiles(
                    exports.value(), context);
                if (!stagedFileCount) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(stagedFileCount).error());
                }
                if (stagedFileCount.value() >
                    Contracts::IProjectMemoryArtifactStore::
                        MaximumOwnedArtifactFilesPerProject) {
                    return Domain::Result<Domain::PathText>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The project-memory artifact file quota was exceeded."));
                }
                auto written = writeAll(pending.handle(), content, context);
                if (!written) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(written).error());
                }
                auto revalidated = validateHandle(
                    exports.value().handle.get(),
                    exports.value().canonicalPath,
                    true);
                if (!revalidated) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(revalidated).error());
                }
                active = WindowsDetail::validateOperationContext(
                    context,
                    std::chrono::steady_clock::now(),
                    "publish a project-memory artifact atomically");
                if (!active) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(active).error());
                }
                auto renamed = renameRelative(
                    pending.handle(), exports.value().handle.get(), finalName);
                if (!renamed) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(renamed).error());
                }
                std::wstring publishedPath = exports.value().canonicalPath;
                publishedPath.push_back(L'\\');
                publishedPath.append(finalName);
                auto validated = validateHandle(
                    pending.handle(), publishedPath, false);
                if (!validated) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(validated).error());
                }
                auto publishedText = WindowsDetail::WindowsPathResolver::toPathText(
                    publishedPath);
                if (!publishedText) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(publishedText).error());
                }
                return Domain::Result<Domain::PathText>::success(
                    std::move(publishedText).value());
            }();
            if (!attempted) {
                auto operationError = std::move(attempted).error();
                auto discarded = pending.discard();
                if (!discarded) {
                    return Domain::Result<Domain::PathText>::failure(
                        std::move(discarded).error());
                }
                if (operationError.code == Domain::ErrorCodes::Conflict) {
                    continue;
                }
                return Domain::Result<Domain::PathText>::failure(
                    std::move(operationError));
            }
            pending.commit();
            return attempted;
        }
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "A unique project-memory artifact filename could not be allocated."));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-memory artifact publication failed safely."));
    }
}

Domain::Result<Domain::ProjectMemoryArtifactDocument>
WindowsProjectMemoryArtifactStore::read(
    const Domain::ProjectId& projectId,
    const Domain::PathText& artifact,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "read project memory artifact");
        if (!active) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(active).error());
        }
        if (maximumBytes == 0U || maximumBytes > MaximumArtifactBytes) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The project-memory artifact read bound is invalid."));
        }
        auto admitted = implementation_->admissionFor(projectId).acquire(
            context, "Read a project-memory artifact");
        if (!admitted) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);
        auto exports = openExportsDirectory(
            *implementation_->applicationPaths,
            projectId,
            false,
            true,
            context);
        if (!exports) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(exports).error());
        }
        auto candidate = resolveArtifactCandidate(
            artifact, exports.value().canonicalPath);
        if (!candidate) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(candidate).error());
        }
        implementation_->discardRetainedOversized(
            projectId, candidate.value().canonicalPath);
        WindowsDetail::RelativeOpenOptions options{};
        options.desiredAccess = GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE;
        // Permit only concurrent readers so stream enumeration can retain a
        // second read handle while all mutation and replacement remains blocked.
        options.shareAccess = FILE_SHARE_READ;
        options.disposition = WindowsDetail::RelativeOpenDisposition::OpenExisting;
        options.objectType = WindowsDetail::RelativeObjectType::File;
        options.sequentialAccess = true;
        auto opened = WindowsDetail::openRelative(
            exports.value().handle.get(), candidate.value().leaf, options);
        if (!opened) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                WindowsDetail::makeWin32Error(
                    "retain a project-memory import artifact",
                    opened.win32Error,
                    Domain::ErrorCodes::InvalidRequest));
        }
        auto validated = validateHandle(
            opened.handle.get(), candidate.value().canonicalPath, false);
        if (!validated) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(validated).error());
        }
        auto streams = validateOnlyDefaultDataStream(candidate.value().canonicalPath);
        if (!streams) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(streams).error());
        }
        auto measuredSize = retainedArtifactSize(opened.handle.get());
        if (!measuredSize) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(measuredSize).error());
        }
        if (measuredSize.value() > maximumBytes ||
            measuredSize.value() > MaximumArtifactBytes) {
            auto retained = implementation_->retainOversized(
                projectId,
                candidate.value().canonicalPath,
                candidate.value().leaf,
                maximumBytes,
                measuredSize.value(),
                std::move(exports.value()),
                std::move(opened.handle));
            if (!retained) {
                return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                    std::move(retained).error());
            }
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "A project-memory artifact exceeds the 32 MiB limit."));
        }
        auto content = readAll(opened.handle.get(), maximumBytes, context);
        if (!content) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(content).error());
        }
        auto canonicalArtifact = WindowsDetail::WindowsPathResolver::toPathText(
            candidate.value().canonicalPath);
        if (!canonicalArtifact) {
            return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
                std::move(canonicalArtifact).error());
        }
        return Domain::Result<Domain::ProjectMemoryArtifactDocument>::success(
            Domain::ProjectMemoryArtifactDocument{
                std::move(canonicalArtifact).value(), std::move(content).value()});
    } catch (...) {
        return Domain::Result<Domain::ProjectMemoryArtifactDocument>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Project-memory artifact retention failed safely."));
    }
}

Domain::Result<Domain::PathText>
WindowsProjectMemoryArtifactStore::quarantineOversized(
    const Domain::ProjectId& projectId,
    const Domain::PathText& artifact,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "quarantine an oversized project-memory artifact");
        if (!active) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(active).error());
        }
        if (maximumBytes == 0U || maximumBytes > MaximumArtifactBytes) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project-memory artifact quarantine bound is invalid."));
        }
        auto admitted = implementation_->admissionFor(projectId).acquire(
            context, "Quarantine an oversized project-memory artifact");
        if (!admitted) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);

        auto canonicalPath = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
            artifact.value());
        if (!canonicalPath) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(canonicalPath).error());
        }
        auto retainedResult = implementation_->takeRetainedOversized(
            projectId, canonicalPath.value(), maximumBytes);
        if (!retainedResult) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(retainedResult).error());
        }
        auto retained = std::move(retainedResult).value();
        auto validated = validateHandle(
            retained.exports.handle.get(), retained.exports.canonicalPath, true);
        if (!validated) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(validated).error());
        }
        validated = validateHandle(
            retained.source.get(), retained.canonicalPath, false);
        if (!validated) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(validated).error());
        }
        auto streams = validateOnlyDefaultDataStream(
            retained.canonicalPath);
        if (!streams) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(streams).error());
        }
        auto size = retainedArtifactSize(retained.source.get());
        if (!size) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(size).error());
        }
        if (size.value() != retained.observedBytes ||
            size.value() <= maximumBytes) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The retained project-memory artifact size changed after read."));
        }
        return moveToQuarantine(
            retained.source.get(),
            retained.exports,
            retained.sourceLeaf,
            *implementation_->uuidGenerator,
            context);
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Oversized project-memory artifact quarantine failed safely."));
    }
}

Domain::Result<Domain::PathText>
WindowsProjectMemoryArtifactStore::quarantineCorrupt(
    const Domain::ProjectId& projectId,
    const Domain::ProjectMemoryArtifactDocument& retainedDocument,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = WindowsDetail::validateOperationContext(
            context,
            std::chrono::steady_clock::now(),
            "quarantine a corrupt project-memory artifact");
        if (!active) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(active).error());
        }
        if (retainedDocument.content.size() > MaximumArtifactBytes) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A retained project-memory artifact exceeds the 32 MiB limit."));
        }
        auto admitted = implementation_->admissionFor(projectId).acquire(
            context, "Quarantine a corrupt project-memory artifact");
        if (!admitted) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);

        auto exports = openExportsDirectory(
            *implementation_->applicationPaths,
            projectId,
            false,
            true,
            context);
        if (!exports) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(exports).error());
        }
        auto candidate = resolveArtifactCandidate(
            retainedDocument.artifact, exports.value().canonicalPath);
        if (!candidate) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(candidate).error());
        }

        WindowsDetail::RelativeOpenOptions sourceOptions{};
        sourceOptions.desiredAccess =
            GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE;
        sourceOptions.shareAccess = FILE_SHARE_READ;
        sourceOptions.disposition =
            WindowsDetail::RelativeOpenDisposition::OpenExisting;
        sourceOptions.objectType = WindowsDetail::RelativeObjectType::File;
        sourceOptions.sequentialAccess = true;
        auto source = WindowsDetail::openRelative(
            exports.value().handle.get(), candidate.value().leaf, sourceOptions);
        if (!source) {
            return Domain::Result<Domain::PathText>::failure(
                WindowsDetail::makeWin32Error(
                    "retain a corrupt project-memory artifact for quarantine",
                    source.win32Error,
                    Domain::ErrorCodes::IntegrityFailure));
        }
        auto validated = validateHandle(
            source.handle.get(), candidate.value().canonicalPath, false);
        if (!validated) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(validated).error());
        }
        auto streams = validateOnlyDefaultDataStream(
            candidate.value().canonicalPath);
        if (!streams) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(streams).error());
        }
        auto compared = compareRetainedContent(
            source.handle.get(), retainedDocument.content, context);
        if (!compared) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(compared).error());
        }

        return moveToQuarantine(
            source.handle.get(),
            exports.value(),
            candidate.value().leaf,
            *implementation_->uuidGenerator,
            context);
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project-memory artifact quarantine failed safely."));
    }
}

} // namespace ForgeConductor::Persistence::Windows
