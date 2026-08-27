#include "AtomicReplaceEngine.h"

#include "OperationContextGuard.h"
#include "RelativeFileOperations.h"
#include "UniqueHandle.h"
#include "UniqueLocalAllocation.h"
#include "Win32Error.h"
#include "WindowsPathResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail
{
namespace
{

constexpr std::size_t IoChunkBytes = 64U * 1024U;
constexpr std::size_t MaximumNativePathCharacters = 32U * 1024U;
constexpr std::size_t MaximumStreamInformationBytes = 64U * 1024U;
constexpr std::size_t TemporaryDirectoryBufferBytes = 16U * 1024U;
constexpr std::size_t MaximumTemporaryEntriesPerScan = 64U;
constexpr ULONG NativeFileRenameInformationEx = 65U;
constexpr ULONG NativeFileIdExtdDirectoryInformation = 60U;
constexpr LONG NativeStatusNoMoreFiles = static_cast<LONG>(0x80000006UL);

constexpr std::wstring_view TemporaryNamePrefix = L".forge-tmp-";
constexpr std::wstring_view TemporaryNameSuffix = L".tmp";
constexpr std::size_t TemporaryNameHexCharacters = 32U;
constexpr std::wstring_view TemporaryNameQuery = L".forge-tmp-*.tmp";

struct NativeIoStatusBlock final
{
    union {
        LONG status;
        void *pointer;
    } result{};
    ULONG_PTR information{};
};

using NtSetInformationFileFunction = LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG,
                                                   ULONG);
using NtQueryInformationFileFunction = LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG,
                                                     ULONG);
struct NativeUnicodeString final
{
    USHORT length{};
    USHORT maximumLength{};
    wchar_t *buffer{};
};
using NtQueryDirectoryFileFunction = LONG(NTAPI *)(HANDLE, HANDLE, void *, void *,
                                                   NativeIoStatusBlock *, void *, ULONG, ULONG,
                                                   BOOLEAN, NativeUnicodeString *, BOOLEAN);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);

struct NativeFileEaInformation final
{
    ULONG eaSize{};
};

[[nodiscard]] Domain::Result<std::uint32_t> queryExtendedAttributeSize(const HANDLE file) noexcept
{
    constexpr ULONG NativeFileEaInformationClass = 7U;
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return Domain::Result<std::uint32_t>::failure(
            makeWin32Error("Load native EA inspection support", ERROR_PROC_NOT_FOUND));
    }
    const auto ntQueryInformationFile = reinterpret_cast<NtQueryInformationFileFunction>(
        ::GetProcAddress(ntdll, "NtQueryInformationFile"));
    const auto rtlNtStatusToDosError = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntQueryInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        return Domain::Result<std::uint32_t>::failure(
            makeWin32Error("Load native EA inspection support", ERROR_PROC_NOT_FOUND));
    }

    NativeFileEaInformation information{};
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntQueryInformationFile(file, &ioStatus, &information,
                                               static_cast<ULONG>(sizeof(information)),
                                               NativeFileEaInformationClass);
    if (status < 0)
    {
        return Domain::Result<std::uint32_t>::failure(
            makeWin32Error("Inspect atomic file extended attributes",
                           static_cast<DWORD>(rtlNtStatusToDosError(status))));
    }
    return Domain::Result<std::uint32_t>::success(information.eaSize);
}

[[nodiscard]] AtomicNativeCallResult setNativeRenameInformation(
    const HANDLE source, void *information, const ULONG informationBytes) noexcept
{
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return {false, ERROR_PROC_NOT_FOUND};
    }
    const auto ntSetInformationFile = reinterpret_cast<NtSetInformationFileFunction>(
        ::GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto rtlNtStatusToDosError = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        return {false, ERROR_PROC_NOT_FOUND};
    }
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSetInformationFile(source, &ioStatus, information, informationBytes,
                                             NativeFileRenameInformationEx);
    if (status >= 0)
    {
        return {true, ERROR_SUCCESS};
    }
    return {false, static_cast<std::uint32_t>(rtlNtStatusToDosError(status))};
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value)
{
    if (value.starts_with(L"\\\\?\\"))
    {
        value.erase(0, 4);
    }
    return value;
}

[[nodiscard]] bool equalPath(const std::wstring_view left, const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

class WindowsAtomicReplaceNativeOperations final : public IAtomicReplaceNativeOperations
{
  public:
    [[nodiscard]] Domain::Result<std::array<std::byte, 16U>> randomBytes() noexcept override
    {
        std::array<std::byte, 16U> bytes{};
        const NTSTATUS status =
            ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                              static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status))
        {
            return Domain::Result<std::array<std::byte, 16U>>::failure(
                makeNtStatusError("generate an atomic temporary-file name", status));
        }
        return Domain::Result<std::array<std::byte, 16U>>::success(bytes);
    }

    void beforeReadOpen(std::wstring_view) noexcept override
    {
    }

    void beforeReplacementVerification() noexcept override
    {
    }

    void beforePublish(std::wstring_view) noexcept override
    {
    }

    [[nodiscard]] AtomicFeatureCallResult queryPosixRenameSupport(
        const HANDLE directory) noexcept override
    {
        DWORD flags{};
        if (::GetVolumeInformationByHandleW(directory, nullptr, 0U, nullptr, nullptr, &flags,
                                            nullptr, 0U) == FALSE)
        {
            return {false, false, static_cast<std::uint32_t>(::GetLastError())};
        }
        return {true, (flags & FILE_SUPPORTS_POSIX_UNLINK_RENAME) != 0U, ERROR_SUCCESS};
    }

    [[nodiscard]] AtomicNativeCallResult renameFile(const HANDLE source,
                                                    const HANDLE destinationDirectory,
                                                    const std::wstring_view destinationName,
                                                    const bool replaceExisting) noexcept override
    {
        try
        {
            if (source == nullptr || source == INVALID_HANDLE_VALUE ||
                destinationDirectory == nullptr || destinationDirectory == INVALID_HANDLE_VALUE ||
                destinationName.empty() ||
                destinationName.find_first_of(L"\\/:") != std::wstring_view::npos ||
                destinationName.size() > (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t))
            {
                return {false, ERROR_INVALID_PARAMETER};
            }

            const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
            const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
            if (informationBytes > (std::numeric_limits<DWORD>::max)())
            {
                return {false, ERROR_FILENAME_EXCED_RANGE};
            }
            std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                               sizeof(std::uint64_t));
            auto *const information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
            std::memset(information, 0, informationBytes);
            information->Flags = replaceExisting ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                                                       FILE_RENAME_FLAG_POSIX_SEMANTICS
                                                 : 0U;
            // The source was created in the retained destination directory, so a
            // simple name and null RootDirectory selects an in-directory rename
            // without reopening a path.
            information->RootDirectory = nullptr;
            information->FileNameLength = static_cast<DWORD>(nameBytes);
            std::memcpy(information->FileName, destinationName.data(), nameBytes);
            information->FileName[destinationName.size()] = L'\0';
            return setNativeRenameInformation(source, information,
                                              static_cast<ULONG>(informationBytes));
        }
        catch (...)
        {
            return {false, ERROR_NOT_ENOUGH_MEMORY};
        }
    }
};

[[nodiscard]] Domain::Error fileError(const std::string_view action,
                                      const DWORD nativeCode) noexcept
{
    switch (nativeCode)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return makeWin32Error(action, nativeCode, Domain::ErrorCodes::RecordNotFound);
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return makeWin32Error(action, nativeCode, Domain::ErrorCodes::Unauthorized);
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return makeWin32Error(action, nativeCode, Domain::ErrorCodes::StorageFull);
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return makeWin32Error(action, nativeCode, Domain::ErrorCodes::Conflict, true);
    case ERROR_CRC:
    case ERROR_FILE_CORRUPT:
    case ERROR_DISK_CORRUPT:
    case ERROR_DELETE_PENDING:
        return makeWin32Error(action, nativeCode, Domain::ErrorCodes::IntegrityFailure);
    default:
        return makeWin32Error(action, nativeCode);
    }
}

[[nodiscard]] Domain::Result<void> verifyOpenedPath(const HANDLE file,
                                                    const std::wstring_view expectedPath,
                                                    const bool expectDirectory) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Inspect opened atomic file", ::GetLastError()));
    }
    const bool isDirectory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        isDirectory != expectDirectory)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                              "Atomic path has an unexpected type or is a reparse point."));
    }

    const DWORD required =
        ::GetFinalPathNameByHandleW(file, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U || required > MaximumNativePathCharacters + 4U)
    {
        return Domain::Result<void>::failure(
            fileError("Resolve opened atomic file", ::GetLastError()));
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written =
        ::GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= buffer.size())
    {
        return Domain::Result<void>::failure(
            fileError("Resolve opened atomic file", ::GetLastError()));
    }
    auto actual =
        withoutExtendedPrefix(std::wstring{buffer.data(), static_cast<std::size_t>(written)});
    if (!equalPath(expectedPath, actual) &&
        !WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, actual))
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                              "Opened atomic file escaped its canonical authority path."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> verifyOpenedFile(const HANDLE file,
                                                    const std::wstring_view expectedPath) noexcept
{
    return verifyOpenedPath(file, expectedPath, false);
}

struct AtomicFileIdentity final
{
    std::uint64_t volumeSerialNumber{};
    std::array<std::byte, 16U> fileId{};

    [[nodiscard]] bool equals(const AtomicFileIdentity &other) const noexcept
    {
        return volumeSerialNumber == other.volumeSerialNumber && fileId == other.fileId;
    }
};

struct OpenedAtomicFile final
{
    UniqueHandle handle;
    AtomicFileIdentity identity;
};

struct AtomicMandatoryLabel final
{
    DWORD mask{};
    BYTE aceFlags{};
    std::vector<std::byte> sid;

    bool operator==(const AtomicMandatoryLabel &) const = default;
};

struct AtomicMetadata final
{
    std::vector<DWORD> daclStorage;
    std::size_t daclBytes{};
    std::vector<std::byte> ownerSid;
    std::vector<std::byte> groupSid;
    std::optional<AtomicMandatoryLabel> mandatoryLabel;
    std::int64_t creationTime{};
    DWORD durableAttributes{FILE_ATTRIBUTE_NORMAL};
    bool nullDacl{};
    SECURITY_DESCRIPTOR_CONTROL daclControl{};

    [[nodiscard]] PACL dacl() noexcept
    {
        return nullDacl ? nullptr : reinterpret_cast<PACL>(daclStorage.data());
    }

    [[nodiscard]] bool equivalentTo(const AtomicMetadata &other) const noexcept
    {
        if (creationTime != other.creationTime || nullDacl != other.nullDacl ||
            durableAttributes != other.durableAttributes || daclControl != other.daclControl ||
            daclBytes != other.daclBytes || ownerSid != other.ownerSid ||
            groupSid != other.groupSid || mandatoryLabel != other.mandatoryLabel)
        {
            return false;
        }
        return nullDacl ||
               std::memcmp(daclStorage.data(), other.daclStorage.data(), daclBytes) == 0;
    }
};

[[nodiscard]] Domain::Result<std::vector<std::byte>> copySid(const PSID sid,
                                                             const std::string_view name) noexcept
{
    try
    {
        if (sid == nullptr || ::IsValidSid(sid) == FALSE)
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Atomic file " + std::string{name} + " SID is missing or invalid."));
        }
        const DWORD bytes = ::GetLengthSid(sid);
        if (bytes == 0U || bytes > SECURITY_MAX_SID_SIZE)
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Atomic file " + std::string{name} + " SID exceeds its validated bound."));
        }
        std::vector<std::byte> result(bytes);
        std::memcpy(result.data(), sid, bytes);
        return Domain::Result<std::vector<std::byte>>::success(std::move(result));
    }
    catch (...)
    {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Atomic file " + std::string{name} + " SID capture failed."));
    }
}

[[nodiscard]] Domain::Result<AtomicFileIdentity> readAtomicFileIdentity(
    const HANDLE file, const std::string_view action) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE)
    {
        return Domain::Result<AtomicFileIdentity>::failure(fileError(action, ::GetLastError()));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
    {
        return Domain::Result<AtomicFileIdentity>::failure(
            Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                              "Atomic file identity belongs to a directory or reparse point."));
    }

    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ==
        FALSE)
    {
        return Domain::Result<AtomicFileIdentity>::failure(fileError(action, ::GetLastError()));
    }
    if (standard.Directory != FALSE || standard.DeletePending != FALSE ||
        standard.NumberOfLinks != 1U)
    {
        return Domain::Result<AtomicFileIdentity>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic storage accepts only single-link files that "
                              "are not delete-pending."));
    }

    auto extendedAttributes = queryExtendedAttributeSize(file);
    if (!extendedAttributes)
    {
        return Domain::Result<AtomicFileIdentity>::failure(std::move(extendedAttributes).error());
    }
    auto supportedExtendedAttributes =
        AtomicReplaceEngine::validateExtendedAttributeSize(extendedAttributes.value());
    if (!supportedExtendedAttributes)
    {
        return Domain::Result<AtomicFileIdentity>::failure(
            std::move(supportedExtendedAttributes).error());
    }

    FILE_ID_INFO information{};
    if (::GetFileInformationByHandleEx(file, FileIdInfo, &information, sizeof(information)) ==
        FALSE)
    {
        return Domain::Result<AtomicFileIdentity>::failure(fileError(action, ::GetLastError()));
    }
    AtomicFileIdentity identity{};
    identity.volumeSerialNumber = information.VolumeSerialNumber;
    std::memcpy(identity.fileId.data(), information.FileId.Identifier, identity.fileId.size());
    return Domain::Result<AtomicFileIdentity>::success(identity);
}

[[nodiscard]] Domain::Result<void> validateDefaultDataStreamOnly(const HANDLE file) noexcept
{
    try
    {
        std::vector<std::uint64_t> storage(MaximumStreamInformationBytes / sizeof(std::uint64_t));
        if (::GetFileInformationByHandleEx(file, FileStreamInfo, storage.data(),
                                           static_cast<DWORD>(MaximumStreamInformationBytes)) ==
            FALSE)
        {
            const DWORD nativeError = ::GetLastError();
            if (nativeError == ERROR_MORE_DATA || nativeError == ERROR_INSUFFICIENT_BUFFER)
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Atomic replacement rejects unbounded alternate-stream metadata."));
            }
            return Domain::Result<void>::failure(
                fileError("Inspect atomic file data streams", nativeError));
        }

        std::size_t offset{};
        std::size_t streamCount{};
        for (;;)
        {
            if (offset + offsetof(FILE_STREAM_INFO, StreamName) > MaximumStreamInformationBytes)
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Atomic file stream metadata exceeded its validated bounds."));
            }
            const auto *const information = reinterpret_cast<const FILE_STREAM_INFO *>(
                reinterpret_cast<const std::byte *>(storage.data()) + offset);
            if ((information->StreamNameLength % sizeof(wchar_t)) != 0U ||
                information->StreamNameLength >
                    MaximumStreamInformationBytes - offset - offsetof(FILE_STREAM_INFO, StreamName))
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Atomic file stream metadata has an invalid name bound."));
            }
            const std::wstring_view streamName{
                information->StreamName,
                static_cast<std::size_t>(information->StreamNameLength) / sizeof(wchar_t)};
            ++streamCount;
            if (streamCount != 1U || (!streamName.empty() && !equalPath(streamName, L"::$DATA")))
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Atomic replacement rejects files with alternate data streams."));
            }
            if (information->NextEntryOffset == 0U)
            {
                break;
            }
            if (information->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) ||
                information->NextEntryOffset > MaximumStreamInformationBytes - offset)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Atomic file stream metadata has an invalid entry bound."));
            }
            offset += information->NextEntryOffset;
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Atomic file stream metadata validation failed."));
    }
}

[[nodiscard]] Domain::Result<AtomicMetadata> captureSupportedMetadata(const HANDLE file) noexcept
{
    try
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                           sizeof(attributes)) == FALSE)
        {
            return Domain::Result<AtomicMetadata>::failure(
                fileError("Inspect atomic file attributes", ::GetLastError()));
        }
        constexpr DWORD SupportedDurableAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE;
        if ((attributes.FileAttributes & ~SupportedDurableAttributes) != 0U)
        {
            return Domain::Result<AtomicMetadata>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure, "Atomic replacement rejects unsupported or "
                                                      "non-durable file attributes."));
        }
        auto streams = validateDefaultDataStreamOnly(file);
        if (!streams)
        {
            return Domain::Result<AtomicMetadata>::failure(std::move(streams).error());
        }

        FILE_BASIC_INFO basic{};
        if (::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) == FALSE)
        {
            return Domain::Result<AtomicMetadata>::failure(
                fileError("Read atomic file creation time", ::GetLastError()));
        }

        PSID owner{};
        PSID group{};
        PACL dacl{};
        PACL sacl{};
        PSECURITY_DESCRIPTOR rawDescriptor{};
        constexpr SECURITY_INFORMATION CapturedSecurity =
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            LABEL_SECURITY_INFORMATION;
        const DWORD securityError = ::GetSecurityInfo(file, SE_FILE_OBJECT, CapturedSecurity,
                                                      &owner, &group, &dacl, &sacl, &rawDescriptor);
        UniqueLocalAllocation<void> descriptor{rawDescriptor};
        if (securityError != ERROR_SUCCESS || !descriptor)
        {
            return Domain::Result<AtomicMetadata>::failure(
                fileError("Read atomic file security metadata", securityError));
        }
        BOOL daclPresent{};
        BOOL daclDefaulted{};
        if (::GetSecurityDescriptorDacl(descriptor.get(), &daclPresent, &dacl, &daclDefaulted) ==
                FALSE ||
            daclPresent == FALSE)
        {
            return Domain::Result<AtomicMetadata>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Atomic file does not expose a preservable DACL."));
        }
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        if (::GetSecurityDescriptorControl(descriptor.get(), &control, &revision) == FALSE)
        {
            return Domain::Result<AtomicMetadata>::failure(
                fileError("Read atomic file DACL control", ::GetLastError()));
        }

        AtomicMetadata metadata{};
        metadata.creationTime = basic.CreationTime.QuadPart;
        metadata.durableAttributes = (attributes.FileAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0U
                                         ? FILE_ATTRIBUTE_ARCHIVE
                                         : FILE_ATTRIBUTE_NORMAL;
        metadata.nullDacl = dacl == nullptr;
        constexpr SECURITY_DESCRIPTOR_CONTROL PreservedDaclControl =
            SE_DACL_DEFAULTED | SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED |
            SE_DACL_PROTECTED;
        metadata.daclControl = control & PreservedDaclControl;
        auto ownerCopy = copySid(owner, "owner");
        if (!ownerCopy)
        {
            return Domain::Result<AtomicMetadata>::failure(std::move(ownerCopy).error());
        }
        metadata.ownerSid = std::move(ownerCopy).value();
        auto groupCopy = copySid(group, "primary group");
        if (!groupCopy)
        {
            return Domain::Result<AtomicMetadata>::failure(std::move(groupCopy).error());
        }
        metadata.groupSid = std::move(groupCopy).value();

        BOOL saclPresent{};
        BOOL saclDefaulted{};
        if (::GetSecurityDescriptorSacl(descriptor.get(), &saclPresent, &sacl, &saclDefaulted) ==
            FALSE)
        {
            return Domain::Result<AtomicMetadata>::failure(
                fileError("Read atomic file mandatory label", ::GetLastError()));
        }
        if (saclPresent != FALSE && sacl != nullptr)
        {
            if (::IsValidAcl(sacl) == FALSE)
            {
                return Domain::Result<AtomicMetadata>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Atomic file mandatory-label ACL is invalid."));
            }
            for (DWORD index = 0U; index < sacl->AceCount; ++index)
            {
                void *rawAce{};
                if (::GetAce(sacl, index, &rawAce) == FALSE || rawAce == nullptr)
                {
                    return Domain::Result<AtomicMetadata>::failure(
                        fileError("Read atomic file mandatory-label ACE", ::GetLastError()));
                }
                const auto *const header = static_cast<const ACE_HEADER *>(rawAce);
                if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                {
                    return Domain::Result<AtomicMetadata>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                          "Atomic LABEL security query returned "
                                          "unsupported SACL metadata."));
                }
                if (metadata.mandatoryLabel.has_value())
                {
                    return Domain::Result<AtomicMetadata>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                          "Atomic file exposes multiple mandatory-label ACEs."));
                }
                const auto *const label = static_cast<const SYSTEM_MANDATORY_LABEL_ACE *>(rawAce);
                constexpr std::size_t LabelSidOffset =
                    offsetof(SYSTEM_MANDATORY_LABEL_ACE, SidStart);
                if (label->Header.AceSize < LabelSidOffset + 8U)
                {
                    return Domain::Result<AtomicMetadata>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                          "Atomic mandatory-label ACE has an invalid size."));
                }
                auto labelSid = copySid(const_cast<DWORD *>(&label->SidStart), "mandatory label");
                if (!labelSid)
                {
                    return Domain::Result<AtomicMetadata>::failure(std::move(labelSid).error());
                }
                if (labelSid.value().size() > label->Header.AceSize - LabelSidOffset)
                {
                    return Domain::Result<AtomicMetadata>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                          "Atomic mandatory-label SID exceeds its ACE bound."));
                }
                metadata.mandatoryLabel = AtomicMandatoryLabel{label->Mask, label->Header.AceFlags,
                                                               std::move(labelSid).value()};
            }
        }
        if (dacl != nullptr)
        {
            if (::IsValidAcl(dacl) == FALSE || dacl->AclSize < sizeof(ACL))
            {
                return Domain::Result<AtomicMetadata>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure, "Atomic file has an invalid DACL."));
            }
            metadata.daclBytes = dacl->AclSize;
            metadata.daclStorage.resize((metadata.daclBytes + sizeof(DWORD) - 1U) / sizeof(DWORD));
            std::memcpy(metadata.daclStorage.data(), dacl, metadata.daclBytes);
        }
        return Domain::Result<AtomicMetadata>::success(std::move(metadata));
    }
    catch (...)
    {
        return Domain::Result<AtomicMetadata>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Atomic file metadata capture failed."));
    }
}

[[nodiscard]] Domain::Result<void> applyAndVerifyMetadata(HANDLE file,
                                                          AtomicMetadata &metadata) noexcept
{
    SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION;
    information |= (metadata.daclControl & SE_DACL_PROTECTED) != 0U
                       ? PROTECTED_DACL_SECURITY_INFORMATION
                       : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD securityError = ::SetSecurityInfo(file, SE_FILE_OBJECT, information, nullptr,
                                                  nullptr, metadata.dacl(), nullptr);
    if (securityError != ERROR_SUCCESS)
    {
        return Domain::Result<void>::failure(fileError("Apply atomic file DACL", securityError));
    }

    FILE_BASIC_INFO basic{};
    basic.CreationTime.QuadPart = metadata.creationTime;
    basic.FileAttributes = metadata.durableAttributes;
    if (::SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic)) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Apply atomic file creation time and attributes", ::GetLastError()));
    }

    auto applied = captureSupportedMetadata(file);
    if (!applied)
    {
        return Domain::Result<void>::failure(std::move(applied).error());
    }
    auto securityIdentity = AtomicReplaceEngine::validateSecurityIdentityEquivalence(
        metadata.ownerSid == applied.value().ownerSid,
        metadata.groupSid == applied.value().groupSid,
        metadata.mandatoryLabel == applied.value().mandatoryLabel);
    if (!securityIdentity)
    {
        return securityIdentity;
    }
    if (!metadata.equivalentTo(applied.value()))
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic staged-file DACL or creation time differs "
                              "from the source file."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> applyAndVerifyNewRecordAttributes(const HANDLE file) noexcept
{
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (::SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic)) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Apply durable atomic file attributes", ::GetLastError()));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Verify durable atomic file attributes", ::GetLastError()));
    }
    if (attributes.FileAttributes != FILE_ATTRIBUTE_NORMAL)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "A newly staged atomic record retained unsupported "
                              "or temporary attributes."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateStagingDirectory(const HANDLE directory) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Inspect atomic staging directory", ::GetLastError()));
    }
    constexpr DWORD RejectedAttributes =
        FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & RejectedAttributes) != 0U)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic staging requires an ordinary, uncompressed, "
                              "unencrypted directory."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::optional<OpenedAtomicFile>> openReplacementTarget(
    const HANDLE parentDirectory, const std::wstring_view targetName,
    const std::wstring_view targetPath) noexcept
{
    RelativeOpenOptions options{};
    options.desiredAccess = FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA | READ_CONTROL;
    // Windows requires delete sharing on an open destination for
    // FILE_RENAME_FLAG_POSIX_SEMANTICS replacement. Write sharing remains denied,
    // so the retained handle owns stable bytes and metadata through publication.
    options.shareAccess = FILE_SHARE_READ | FILE_SHARE_DELETE;
    options.disposition = RelativeOpenDisposition::OpenExisting;
    options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    options.objectType = RelativeObjectType::File;
    options.sequentialAccess = true;
    auto relative = openRelative(parentDirectory, targetName, options);
    if (!relative)
    {
        if (relative.win32Error == ERROR_FILE_NOT_FOUND ||
            relative.win32Error == ERROR_PATH_NOT_FOUND)
        {
            return Domain::Result<std::optional<OpenedAtomicFile>>::success(std::nullopt);
        }
        return Domain::Result<std::optional<OpenedAtomicFile>>::failure(
            fileError("Open atomic replacement target", relative.win32Error));
    }

    auto verifiedPath = verifyOpenedFile(relative.handle.get(), targetPath);
    if (!verifiedPath)
    {
        return Domain::Result<std::optional<OpenedAtomicFile>>::failure(
            std::move(verifiedPath).error());
    }
    auto identity =
        readAtomicFileIdentity(relative.handle.get(), "Read atomic replacement target identity");
    if (!identity)
    {
        return Domain::Result<std::optional<OpenedAtomicFile>>::failure(
            std::move(identity).error());
    }
    return Domain::Result<std::optional<OpenedAtomicFile>>::success(
        OpenedAtomicFile{std::move(relative.handle), std::move(identity).value()});
}

[[nodiscard]] Domain::Result<void> verifyCurrentIdentity(
    const HANDLE parentDirectory, const std::wstring_view name,
    const std::optional<AtomicFileIdentity> &expectedIdentity) noexcept
{
    RelativeOpenOptions options{};
    options.desiredAccess = FILE_READ_ATTRIBUTES | FILE_READ_EA;
    options.shareAccess = FILE_SHARE_READ;
    options.disposition = RelativeOpenDisposition::OpenExisting;
    options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    options.objectType = RelativeObjectType::File;
    auto opened = openRelative(parentDirectory, name, options);
    if (!opened)
    {
        if (!expectedIdentity.has_value() && (opened.win32Error == ERROR_FILE_NOT_FOUND ||
                                              opened.win32Error == ERROR_PATH_NOT_FOUND))
        {
            return Domain::Result<void>::success();
        }
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "Atomic destination identity changed before handle-relative commit.", true));
    }
    if (!expectedIdentity.has_value())
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::Conflict,
                              "Atomic destination appeared before handle-relative commit.", true));
    }
    auto current =
        readAtomicFileIdentity(opened.handle.get(), "Recheck atomic destination identity");
    if (!current)
    {
        return Domain::Result<void>::failure(std::move(current).error());
    }
    if (!current.value().equals(expectedIdentity.value()))
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "Atomic destination identity changed before handle-relative commit.", true));
    }
    return Domain::Result<void>::success();
}

class PendingTemporaryFile final
{
  public:
    PendingTemporaryFile(std::wstring path, std::wstring name, AtomicFileIdentity identity,
                         UniqueHandle handle) noexcept
        : path_{std::move(path)}, name_{std::move(name)}, identity_{identity},
          handle_{std::move(handle)}
    {
    }

    PendingTemporaryFile(const PendingTemporaryFile &) = delete;
    PendingTemporaryFile &operator=(const PendingTemporaryFile &) = delete;
    PendingTemporaryFile(PendingTemporaryFile &&other) noexcept
        : path_{std::move(other.path_)}, name_{std::move(other.name_)}, identity_{other.identity_},
          handle_{std::move(other.handle_)},
          cleanupRequired_{std::exchange(other.cleanupRequired_, false)}
    {
    }
    PendingTemporaryFile &operator=(PendingTemporaryFile &&) = delete;

    ~PendingTemporaryFile()
    {
        if (cleanupRequired_ && handle_)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            static_cast<void>(::SetFileInformationByHandle(handle_.get(), FileDispositionInfo,
                                                           &disposition, sizeof(disposition)));
        }
    }

    [[nodiscard]] HANDLE handle() const noexcept
    {
        return handle_.get();
    }
    [[nodiscard]] const AtomicFileIdentity &identity() const noexcept
    {
        return identity_;
    }
    void markCommitted() noexcept
    {
        cleanupRequired_ = false;
    }

  private:
    std::wstring path_;
    std::wstring name_;
    AtomicFileIdentity identity_;
    UniqueHandle handle_;
    bool cleanupRequired_{true};
};

[[nodiscard]] std::wstring temporaryName(const std::array<std::byte, 16U> &random)
{
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring name{L".forge-tmp-"};
    name.reserve(name.size() + random.size() * 2U + 4U);
    for (const std::byte item : random)
    {
        const unsigned int value = std::to_integer<unsigned int>(item);
        name.push_back(Hex[(value >> 4U) & 0x0fU]);
        name.push_back(Hex[value & 0x0fU]);
    }
    name.append(L".tmp");
    return name;
}

[[nodiscard]] bool isHexadecimal(const wchar_t value) noexcept
{
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

[[nodiscard]] bool hasReservedTemporaryNameShape(const std::wstring_view name) noexcept
{
    const std::size_t expectedLength =
        TemporaryNamePrefix.size() + TemporaryNameHexCharacters + TemporaryNameSuffix.size();
    if (name.size() != expectedLength ||
        ::CompareStringOrdinal(name.data(), static_cast<int>(TemporaryNamePrefix.size()),
                               TemporaryNamePrefix.data(),
                               static_cast<int>(TemporaryNamePrefix.size()), TRUE) != CSTR_EQUAL ||
        ::CompareStringOrdinal(name.data() + expectedLength - TemporaryNameSuffix.size(),
                               static_cast<int>(TemporaryNameSuffix.size()),
                               TemporaryNameSuffix.data(),
                               static_cast<int>(TemporaryNameSuffix.size()), TRUE) != CSTR_EQUAL)
    {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(TemporaryNamePrefix.size()),
                       name.end() - static_cast<std::ptrdiff_t>(TemporaryNameSuffix.size()),
                       isHexadecimal);
}

[[nodiscard]] bool isExactTemporaryName(const std::wstring_view name) noexcept
{
    if (!hasReservedTemporaryNameShape(name) || !name.starts_with(TemporaryNamePrefix) ||
        !name.ends_with(TemporaryNameSuffix))
    {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(TemporaryNamePrefix.size()),
                       name.end() - static_cast<std::ptrdiff_t>(TemporaryNameSuffix.size()),
                       [](const wchar_t value) noexcept {
                           return (value >= L'0' && value <= L'9') ||
                                  (value >= L'a' && value <= L'f');
                       });
}

struct ScavengerCandidate final
{
    std::wstring name;
    std::array<std::byte, 16U> fileId{};
};

struct ScavengerScan final
{
    std::vector<ScavengerCandidate> candidates;
    std::size_t reservedAliases{};
    bool truncated{};
};

[[nodiscard]] Domain::Result<ScavengerScan> scanTemporaryCandidates(
    const HANDLE parentDirectory, const Domain::OperationContext &context) noexcept
{
    try
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntQueryDirectoryFile =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<NtQueryDirectoryFileFunction>(
                                   ::GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        const auto rtlNtStatusToDosError =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                                   ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntQueryDirectoryFile == nullptr || rtlNtStatusToDosError == nullptr)
        {
            return Domain::Result<ScavengerScan>::failure(
                fileError("Load atomic temporary-file recovery support", ERROR_PROC_NOT_FOUND));
        }

        NativeUnicodeString query{};
        query.length = static_cast<USHORT>(TemporaryNameQuery.size() * sizeof(wchar_t));
        query.maximumLength = query.length;
        query.buffer = const_cast<wchar_t *>(TemporaryNameQuery.data());

        ScavengerScan scan{};
        scan.candidates.reserve(MaximumTemporaryEntriesPerScan);
        std::array<std::uint64_t, TemporaryDirectoryBufferBytes / sizeof(std::uint64_t)> buffer{};
        bool firstQuery{true};
        std::size_t entriesObserved{};
        for (std::size_t queryCount = 0U; queryCount < MaximumTemporaryEntriesPerScan + 2U;
             ++queryCount)
        {
            auto validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                         "Atomic temporary-file recovery");
            if (!validContext)
            {
                return Domain::Result<ScavengerScan>::failure(std::move(validContext).error());
            }

            NativeIoStatusBlock ioStatus{};
            const LONG status = ntQueryDirectoryFile(
                parentDirectory, nullptr, nullptr, nullptr, &ioStatus, buffer.data(),
                static_cast<ULONG>(TemporaryDirectoryBufferBytes),
                NativeFileIdExtdDirectoryInformation, FALSE, firstQuery ? &query : nullptr,
                firstQuery ? TRUE : FALSE);
            firstQuery = false;
            if (status == NativeStatusNoMoreFiles)
            {
                return Domain::Result<ScavengerScan>::success(std::move(scan));
            }
            if (status < 0)
            {
                const DWORD nativeError = static_cast<DWORD>(rtlNtStatusToDosError(status));
                if (nativeError == ERROR_NO_MORE_FILES || nativeError == ERROR_FILE_NOT_FOUND)
                {
                    return Domain::Result<ScavengerScan>::success(std::move(scan));
                }
                return Domain::Result<ScavengerScan>::failure(
                    fileError("Enumerate atomic temporary files", nativeError));
            }
            if (ioStatus.information == 0U)
            {
                return Domain::Result<ScavengerScan>::success(std::move(scan));
            }
            if (ioStatus.information > TemporaryDirectoryBufferBytes)
            {
                return Domain::Result<ScavengerScan>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Atomic temporary-file enumeration exceeded its native buffer."));
            }

            std::size_t offset{};
            for (;;)
            {
                validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                        "Atomic temporary-file recovery");
                if (!validContext)
                {
                    return Domain::Result<ScavengerScan>::failure(std::move(validContext).error());
                }
                if (offset + offsetof(FILE_ID_EXTD_DIR_INFO, FileName) > ioStatus.information)
                {
                    return Domain::Result<ScavengerScan>::failure(Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Atomic temporary-file enumeration returned an invalid entry."));
                }
                const auto *const information = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO *>(
                    reinterpret_cast<const std::byte *>(buffer.data()) + offset);
                if ((information->FileNameLength % sizeof(wchar_t)) != 0U ||
                    information->FileNameLength >
                        ioStatus.information - offset - offsetof(FILE_ID_EXTD_DIR_INFO, FileName))
                {
                    return Domain::Result<ScavengerScan>::failure(Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Atomic temporary-file enumeration returned an invalid name."));
                }
                ++entriesObserved;
                if (entriesObserved > MaximumTemporaryEntriesPerScan)
                {
                    scan.truncated = true;
                    return Domain::Result<ScavengerScan>::success(std::move(scan));
                }
                const std::wstring_view name{information->FileName,
                                             static_cast<std::size_t>(information->FileNameLength) /
                                                 sizeof(wchar_t)};
                if (isExactTemporaryName(name))
                {
                    ScavengerCandidate candidate{};
                    candidate.name.assign(name);
                    std::memcpy(candidate.fileId.data(), information->FileId.Identifier,
                                candidate.fileId.size());
                    scan.candidates.push_back(std::move(candidate));
                }
                else if (hasReservedTemporaryNameShape(name))
                {
                    ++scan.reservedAliases;
                }

                if (information->NextEntryOffset == 0U)
                {
                    break;
                }
                if (information->NextEntryOffset < offsetof(FILE_ID_EXTD_DIR_INFO, FileName) ||
                    information->NextEntryOffset > ioStatus.information - offset)
                {
                    return Domain::Result<ScavengerScan>::failure(Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Atomic temporary-file enumeration returned an invalid chain."));
                }
                offset += information->NextEntryOffset;
            }
        }
        scan.truncated = true;
        return Domain::Result<ScavengerScan>::success(std::move(scan));
    }
    catch (...)
    {
        return Domain::Result<ScavengerScan>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Atomic temporary-file enumeration failed at the Windows boundary."));
    }
}

enum class ScavengerDisposition : unsigned char
{
    Deleted,
    Missing,
    Retained
};

[[nodiscard]] Domain::Result<ScavengerDisposition> scavengeCandidate(
    const HANDLE parentDirectory, const ScavengerCandidate &candidate) noexcept
{
    RelativeOpenOptions options{};
    options.desiredAccess = FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE;
    options.shareAccess = 0U;
    options.disposition = RelativeOpenDisposition::OpenExisting;
    options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    options.objectType = RelativeObjectType::File;
    auto opened = openRelative(parentDirectory, candidate.name, options);
    if (!opened)
    {
        if (opened.win32Error == ERROR_FILE_NOT_FOUND || opened.win32Error == ERROR_PATH_NOT_FOUND)
        {
            return Domain::Result<ScavengerDisposition>::success(ScavengerDisposition::Missing);
        }
        if (opened.win32Error == ERROR_SHARING_VIOLATION ||
            opened.win32Error == ERROR_LOCK_VIOLATION)
        {
            return Domain::Result<ScavengerDisposition>::success(ScavengerDisposition::Retained);
        }
        if (opened.win32Error == ERROR_ACCESS_DENIED)
        {
            auto directoryOptions = options;
            directoryOptions.objectType = RelativeObjectType::Directory;
            auto directory = openRelative(parentDirectory, candidate.name, directoryOptions);
            if (directory || directory.win32Error != ERROR_DIRECTORY)
            {
                return Domain::Result<ScavengerDisposition>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Atomic temporary-file recovery could not "
                                      "establish that an access-denied "
                                      "object is a regular active stage."));
            }
            return Domain::Result<ScavengerDisposition>::success(ScavengerDisposition::Retained);
        }
        if (opened.win32Error == ERROR_DELETE_PENDING || opened.win32Error == ERROR_DIRECTORY)
        {
            return Domain::Result<ScavengerDisposition>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Atomic temporary-file recovery encountered a "
                                  "non-regular or delete-pending "
                                  "object."));
        }
        return Domain::Result<ScavengerDisposition>::failure(
            fileError("Open stale atomic temporary file", opened.win32Error));
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(opened.handle.get(), FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE)
    {
        return Domain::Result<ScavengerDisposition>::failure(
            fileError("Inspect stale atomic temporary file", ::GetLastError()));
    }
    constexpr DWORD PermittedStagingAttributes =
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY;
    if ((attributes.FileAttributes & ~PermittedStagingAttributes) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
    {
        return Domain::Result<ScavengerDisposition>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure, "Atomic temporary-file recovery rejected a "
                                                  "non-regular staged object."));
    }

    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(opened.handle.get(), FileStandardInfo, &standard,
                                       sizeof(standard)) == FALSE)
    {
        return Domain::Result<ScavengerDisposition>::failure(
            fileError("Inspect stale atomic temporary links", ::GetLastError()));
    }
    if (standard.Directory != FALSE || standard.DeletePending != FALSE ||
        standard.NumberOfLinks != 1U)
    {
        return Domain::Result<ScavengerDisposition>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic temporary-file recovery rejects directories, hard links, and "
                              "delete-pending objects."));
    }
    auto eaSize = queryExtendedAttributeSize(opened.handle.get());
    if (!eaSize)
    {
        return Domain::Result<ScavengerDisposition>::failure(std::move(eaSize).error());
    }
    auto supportedEa = AtomicReplaceEngine::validateExtendedAttributeSize(eaSize.value());
    if (!supportedEa)
    {
        return Domain::Result<ScavengerDisposition>::failure(std::move(supportedEa).error());
    }

    FILE_ID_INFO identity{};
    if (::GetFileInformationByHandleEx(opened.handle.get(), FileIdInfo, &identity,
                                       sizeof(identity)) == FALSE)
    {
        return Domain::Result<ScavengerDisposition>::failure(
            fileError("Identify stale atomic temporary file", ::GetLastError()));
    }
    if (std::memcmp(candidate.fileId.data(), identity.FileId.Identifier, candidate.fileId.size()) !=
        0)
    {
        return Domain::Result<ScavengerDisposition>::success(ScavengerDisposition::Retained);
    }

    FILE_DISPOSITION_INFO disposition{TRUE};
    if (::SetFileInformationByHandle(opened.handle.get(), FileDispositionInfo, &disposition,
                                     sizeof(disposition)) == FALSE)
    {
        return Domain::Result<ScavengerDisposition>::failure(
            fileError("Delete stale atomic temporary file", ::GetLastError()));
    }
    return Domain::Result<ScavengerDisposition>::success(ScavengerDisposition::Deleted);
}

[[nodiscard]] Domain::Result<void> scavengeTemporaryFiles(
    const HANDLE parentDirectory, const std::size_t requiredTemporarySlots,
    const Domain::OperationContext &context) noexcept
{
    auto scan = scanTemporaryCandidates(parentDirectory, context);
    if (!scan)
    {
        return Domain::Result<void>::failure(std::move(scan).error());
    }

    std::size_t retained = scan.value().reservedAliases;
    std::size_t deleted{};
    for (const auto &candidate : scan.value().candidates)
    {
        auto validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                     "Atomic temporary-file recovery");
        if (!validContext)
        {
            return validContext;
        }
        if (deleted >= AtomicReplaceEngine::MaximumStaleTemporaryDeletesPerOperation)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Atomic stale temporary-file cleanup exceeded its deletion bound."));
        }
        auto disposition = scavengeCandidate(parentDirectory, candidate);
        if (!disposition)
        {
            return Domain::Result<void>::failure(std::move(disposition).error());
        }
        if (disposition.value() == ScavengerDisposition::Deleted)
        {
            ++deleted;
        }
        else if (disposition.value() == ScavengerDisposition::Retained)
        {
            ++retained;
        }
    }
    if (scan.value().truncated)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                              "Atomic temporary-file recovery exceeded its enumeration bound."));
    }
    if (retained > AtomicReplaceEngine::MaximumRetainedTemporaryFiles ||
        requiredTemporarySlots > AtomicReplaceEngine::MaximumRetainedTemporaryFiles - retained)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "The bounded atomic temporary-file namespace is currently exhausted.", true));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<PendingTemporaryFile> createTemporaryFile(
    const std::wstring_view targetPath, const HANDLE parentDirectory, const bool metadataRequired,
    IAtomicReplaceNativeOperations &nativeOperations) noexcept
{
    try
    {
        const std::size_t separator = targetPath.find_last_of(L'\\');
        if (separator == std::wstring_view::npos)
        {
            return Domain::Result<PendingTemporaryFile>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "Atomic replacement target has no parent directory."));
        }
        const std::wstring parent{targetPath.substr(0, separator)};
        for (std::size_t attempt = 0; attempt < AtomicReplaceEngine::MaximumTemporaryNameAttempts;
             ++attempt)
        {
            auto random = nativeOperations.randomBytes();
            if (!random)
            {
                return Domain::Result<PendingTemporaryFile>::failure(std::move(random).error());
            }
            auto name = temporaryName(random.value());
            std::wstring candidate = parent;
            candidate.push_back(L'\\');
            candidate.append(name);
            if (candidate.size() > MaximumNativePathCharacters)
            {
                return Domain::Result<PendingTemporaryFile>::failure(
                    Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                      "Atomic temporary path exceeds the Windows path bound."));
            }

            RelativeOpenOptions options{};
            options.desiredAccess = FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                                    FILE_READ_EA | DELETE;
            if (metadataRequired)
            {
                options.desiredAccess |= READ_CONTROL | WRITE_DAC;
            }
            options.shareAccess = 0U;
            options.disposition = RelativeOpenDisposition::CreateNew;
            options.fileAttributes = FILE_ATTRIBUTE_TEMPORARY;
            options.objectType = RelativeObjectType::File;
            options.writeThrough = true;
            auto relative = openRelative(parentDirectory, name, options);
            if (relative)
            {
                auto identity =
                    readAtomicFileIdentity(relative.handle.get(), "Read atomic temporary identity");
                if (!identity)
                {
                    return Domain::Result<PendingTemporaryFile>::failure(
                        std::move(identity).error());
                }
                PendingTemporaryFile pending{candidate, std::move(name),
                                             std::move(identity).value(),
                                             std::move(relative.handle)};
                auto verified = verifyOpenedFile(pending.handle(), candidate);
                if (!verified)
                {
                    return Domain::Result<PendingTemporaryFile>::failure(
                        std::move(verified).error());
                }
                return Domain::Result<PendingTemporaryFile>::success(std::move(pending));
            }
            if (relative.win32Error != ERROR_FILE_EXISTS &&
                relative.win32Error != ERROR_ALREADY_EXISTS &&
                relative.win32Error != ERROR_SHARING_VIOLATION &&
                relative.win32Error != ERROR_LOCK_VIOLATION &&
                relative.win32Error != ERROR_ACCESS_DENIED)
            {
                return Domain::Result<PendingTemporaryFile>::failure(
                    fileError("Create atomic temporary file", relative.win32Error));
            }
        }
        return Domain::Result<PendingTemporaryFile>::failure(
            Domain::makeError(Domain::ErrorCodes::Conflict,
                              "Atomic temporary-name collision retry bound was exhausted.", true));
    }
    catch (...)
    {
        return Domain::Result<PendingTemporaryFile>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Atomic temporary file creation failed."));
    }
}

[[nodiscard]] Domain::Result<void> writeBytes(const HANDLE destination,
                                              const std::span<const std::byte> content,
                                              const Domain::OperationContext &context,
                                              const std::string_view action) noexcept
{
    std::size_t offset{};
    while (offset < content.size())
    {
        auto validContext =
            validateOperationContext(context, std::chrono::steady_clock::now(), action);
        if (!validContext)
        {
            return validContext;
        }
        const auto count = (std::min)(IoChunkBytes, content.size() - offset);
        DWORD written{};
        if (::WriteFile(destination, content.data() + offset, static_cast<DWORD>(count), &written,
                        nullptr) == FALSE)
        {
            return Domain::Result<void>::failure(
                fileError("Write atomic temporary file", ::GetLastError()));
        }
        if (written == 0U || written > count)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Atomic temporary file write made invalid progress."));
        }
        offset += written;
    }
    if (::FlushFileBuffers(destination) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Flush atomic temporary file", ::GetLastError()));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> copyAndFlush(const HANDLE source, const HANDLE destination,
                                                const Domain::OperationContext &context) noexcept
{
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(source, &size) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Size atomic backup source", ::GetLastError()));
    }
    if (size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > AtomicReplaceEngine::MaximumContentBytes)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                              "Atomic backup source exceeds the 32 MiB storage bound."));
    }
    LARGE_INTEGER beginning{};
    if (::SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Seek atomic backup source", ::GetLastError()));
    }

    std::array<std::byte, IoChunkBytes> buffer{};
    std::uint64_t copied{};
    for (;;)
    {
        auto validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                     "Atomic backup copy");
        if (!validContext)
        {
            return validContext;
        }
        DWORD read{};
        if (::ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
            FALSE)
        {
            return Domain::Result<void>::failure(
                fileError("Read atomic backup source", ::GetLastError()));
        }
        if (read == 0U)
        {
            break;
        }
        copied += read;
        if (copied > AtomicReplaceEngine::MaximumContentBytes ||
            copied > static_cast<std::uint64_t>(size.QuadPart))
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Atomic backup source changed while its stable handle was copied."));
        }
        std::size_t offset{};
        while (offset < read)
        {
            DWORD written{};
            if (::WriteFile(destination, buffer.data() + offset, static_cast<DWORD>(read - offset),
                            &written, nullptr) == FALSE)
            {
                return Domain::Result<void>::failure(
                    fileError("Write atomic backup temporary file", ::GetLastError()));
            }
            if (written == 0U || written > read - offset)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Atomic backup temporary write made invalid progress."));
            }
            offset += written;
        }
    }
    if (copied != static_cast<std::uint64_t>(size.QuadPart))
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic backup source size changed while its stable "
                              "handle was copied."));
    }
    if (::FlushFileBuffers(destination) == FALSE)
    {
        return Domain::Result<void>::failure(
            fileError("Flush atomic backup temporary file", ::GetLastError()));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> publishTemporary(
    PendingTemporaryFile &temporary, const HANDLE parentDirectory,
    const std::wstring_view destinationName, const bool replaceExisting,
    const std::optional<AtomicFileIdentity> &expectedDestinationIdentity,
    const Domain::OperationContext &context, const std::string_view action,
    IAtomicReplaceNativeOperations &nativeOperations) noexcept
{
    auto current = readAtomicFileIdentity(temporary.handle(), "Verify atomic staged identity");
    if (!current)
    {
        return Domain::Result<void>::failure(std::move(current).error());
    }
    if (!current.value().equals(temporary.identity()))
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::Conflict,
                              "Atomic staged-file identity changed before commit.", true));
    }
    auto destination =
        verifyCurrentIdentity(parentDirectory, destinationName, expectedDestinationIdentity);
    if (!destination)
    {
        return destination;
    }
    nativeOperations.beforePublish(destinationName);
    auto validContext = validateOperationContext(context, std::chrono::steady_clock::now(), action);
    if (!validContext)
    {
        return validContext;
    }
    current = readAtomicFileIdentity(temporary.handle(), "Final atomic staged identity check");
    if (!current)
    {
        return Domain::Result<void>::failure(std::move(current).error());
    }
    if (!current.value().equals(temporary.identity()))
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::Conflict,
                              "Atomic staged-file identity changed at the commit boundary.", true));
    }
    validContext = validateOperationContext(context, std::chrono::steady_clock::now(), action);
    if (!validContext)
    {
        return validContext;
    }
    const auto renamed = nativeOperations.renameFile(temporary.handle(), parentDirectory,
                                                     destinationName, replaceExisting);
    if (!renamed.succeeded)
    {
        return Domain::Result<void>::failure(
            fileError(action, static_cast<DWORD>(renamed.errorCode)));
    }
    temporary.markCommitted();
    return Domain::Result<void>::success();
}

[[nodiscard]] std::wstring_view leafName(const std::wstring_view path) noexcept
{
    const std::size_t separator = path.find_last_of(L'\\');
    return separator == std::wstring_view::npos ? std::wstring_view{} : path.substr(separator + 1U);
}

} // namespace

Domain::Result<void> AtomicReplaceEngine::validateExtendedAttributeSize(
    const std::uint32_t size) noexcept
{
    if (size != 0U)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic storage rejects files with NTFS extended attributes."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<void> AtomicReplaceEngine::validateSecurityIdentityEquivalence(
    const bool ownerMatches, const bool groupMatches, const bool mandatoryLabelMatches) noexcept
{
    if (!ownerMatches || !groupMatches || !mandatoryLabelMatches)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "Atomic staged owner, primary group, or mandatory "
                              "label differs from the target."));
    }
    return Domain::Result<void>::success();
}

AtomicReplaceEngine::AtomicReplaceEngine()
    : ownedNativeOperations_{std::make_unique<WindowsAtomicReplaceNativeOperations>()},
      nativeOperations_{ownedNativeOperations_.get()}
{
}

AtomicReplaceEngine::AtomicReplaceEngine(IAtomicReplaceNativeOperations &nativeOperations) noexcept
    : nativeOperations_{&nativeOperations}
{
}

AtomicReplaceEngine::~AtomicReplaceEngine() = default;

Domain::Result<std::vector<std::byte>> AtomicReplaceEngine::read(
    const Contracts::AuthorizedPath &path, const std::size_t maximumBytes,
    const Domain::OperationContext &context) noexcept
{
    try
    {
        if (maximumBytes > MaximumContentBytes)
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded, "Atomic file read bound exceeds 32 MiB."));
        }
        auto lease = executor_.acquire(context, "Atomic file read");
        if (!lease)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(lease).error());
        }
        auto resolved = WindowsPathResolver::resolveAnchoredAuthorizedPath(
            path, Domain::FileAccess::Read, MissingPathPolicy::Reject,
            AnchorSharePolicy::DenyConcurrentWrite);
        if (!resolved)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(resolved).error());
        }
        auto validContext =
            validateOperationContext(context, std::chrono::steady_clock::now(), "Atomic file read");
        if (!validContext)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(validContext).error());
        }

        const auto &targetPath = resolved.value().canonicalPath();
        const std::wstring targetName{leafName(targetPath)};
        if (targetName.empty())
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "Atomic read target has no file name."));
        }
        if (hasReservedTemporaryNameShape(targetName))
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "The atomic temporary-file namespace is reserved "
                                  "for crash recovery."));
        }

        nativeOperations_->beforeReadOpen(targetName);
        validContext =
            validateOperationContext(context, std::chrono::steady_clock::now(), "Atomic file read");
        if (!validContext)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(validContext).error());
        }
        auto verifiedAnchors = resolved.value().revalidateDirectoryAnchors();
        if (!verifiedAnchors)
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(verifiedAnchors).error());
        }

        RelativeOpenOptions options{};
        options.desiredAccess = FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_DELETE;
        options.disposition = RelativeOpenDisposition::OpenExisting;
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = RelativeObjectType::File;
        options.sequentialAccess = true;
        auto relative = openRelative(resolved.value().parentDirectoryHandle(), targetName, options);
        if (!relative)
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                fileError("Open atomic file", relative.win32Error));
        }
        auto opened = verifyOpenedFile(relative.handle.get(), targetPath);
        if (!opened)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(opened).error());
        }
        auto identity = readAtomicFileIdentity(relative.handle.get(), "Validate atomic read file");
        if (!identity)
        {
            return Domain::Result<std::vector<std::byte>>::failure(std::move(identity).error());
        }

        LARGE_INTEGER size{};
        if (::GetFileSizeEx(relative.handle.get(), &size) == FALSE)
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                fileError("Size atomic file", ::GetLastError()));
        }
        if (size.QuadPart < 0)
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure, "Atomic file reported a negative size."));
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "Atomic file exceeds the caller's read bound."));
        }

        std::vector<std::byte> content;
        content.reserve(static_cast<std::size_t>(size.QuadPart));
        std::array<std::byte, IoChunkBytes> buffer{};
        for (;;)
        {
            validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                    "Atomic file read");
            if (!validContext)
            {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(validContext).error());
            }
            const auto remaining = maximumBytes - content.size();
            const auto requested = (std::min)(buffer.size(), remaining == 0U ? 1U : remaining);
            DWORD read{};
            if (::ReadFile(relative.handle.get(), buffer.data(), static_cast<DWORD>(requested),
                           &read, nullptr) == FALSE)
            {
                const DWORD nativeError = ::GetLastError();
                if (nativeError == ERROR_HANDLE_EOF)
                {
                    break;
                }
                return Domain::Result<std::vector<std::byte>>::failure(
                    fileError("Read atomic file", nativeError));
            }
            if (read == 0U)
            {
                break;
            }
            if (read > remaining)
            {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                      "Atomic file grew beyond the caller's read bound."));
            }
            content.insert(content.end(), buffer.begin(), buffer.begin() + read);
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(content));
    }
    catch (...)
    {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Atomic file read failed at the Windows boundary."));
    }
}

Domain::Result<void> AtomicReplaceEngine::replace(const Contracts::AuthorizedPath &path,
                                                  const std::span<const std::byte> content,
                                                  const bool retainBackup,
                                                  const Domain::OperationContext &context) noexcept
{
    try
    {
        if (content.size() > MaximumContentBytes)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Atomic replacement exceeds 32 MiB."));
        }
        if (path.access() != Domain::FileAccess::Write &&
            path.access() != Domain::FileAccess::Create)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::Unauthorized,
                                  "Atomic replacement requires write or create authority."));
        }
        auto lease = executor_.acquire(context, "Atomic file replace");
        if (!lease)
        {
            return Domain::Result<void>::failure(std::move(lease).error());
        }
        auto resolved = WindowsPathResolver::resolveAnchoredAuthorizedPath(
            path, path.access(), MissingPathPolicy::AllowLeaf,
            AnchorSharePolicy::DenyConcurrentWrite);
        if (!resolved)
        {
            return Domain::Result<void>::failure(std::move(resolved).error());
        }
        auto validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                     "Atomic file replacement validation");
        if (!validContext)
        {
            return validContext;
        }

        const auto &targetPath = resolved.value().canonicalPath();
        const std::wstring targetName{leafName(targetPath)};
        if (targetName.empty())
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "Atomic replacement target has no file name."));
        }
        if (hasReservedTemporaryNameShape(targetName))
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "The atomic temporary-file namespace is reserved "
                                  "for crash recovery."));
        }
        const HANDLE parentDirectory = resolved.value().parentDirectoryHandle();
        auto stagingDirectory = validateStagingDirectory(parentDirectory);
        if (!stagingDirectory)
        {
            return stagingDirectory;
        }

        auto target = openReplacementTarget(parentDirectory, targetName, targetPath);
        if (!target)
        {
            return Domain::Result<void>::failure(std::move(target).error());
        }
        const bool existing = target.value().has_value();
        if (!existing && path.access() == Domain::FileAccess::Write)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound,
                "Atomic write target does not exist; create authority is required."));
        }
        if (existing && path.access() == Domain::FileAccess::Create)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "Atomic create authority cannot overwrite an "
                                                  "existing file; write authority is "
                                                  "required."));
        }

        std::optional<AtomicMetadata> targetMetadata;
        if (existing)
        {
            auto captured = captureSupportedMetadata(target.value()->handle.get());
            if (!captured)
            {
                return Domain::Result<void>::failure(std::move(captured).error());
            }
            targetMetadata.emplace(std::move(captured).value());

            const auto feature = nativeOperations_->queryPosixRenameSupport(parentDirectory);
            if (!feature.succeeded)
            {
                return Domain::Result<void>::failure(fileError(
                    "Query atomic POSIX rename support", static_cast<DWORD>(feature.errorCode)));
            }
            if (!feature.supported)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                                      "The storage volume does not support safe "
                                      "handle-relative replacement."));
            }
        }

        std::wstring backupPath;
        std::wstring backupName;
        std::optional<OpenedAtomicFile> backup;
        if (existing && retainBackup)
        {
            backupPath = targetPath;
            backupPath.append(L".bak");
            backupName = targetName;
            backupName.append(L".bak");
            if (backupPath.size() > MaximumNativePathCharacters)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                      "Atomic backup path exceeds the Windows path bound."));
            }
            auto openedBackup = openReplacementTarget(parentDirectory, backupName, backupPath);
            if (!openedBackup)
            {
                return Domain::Result<void>::failure(std::move(openedBackup).error());
            }
            backup = std::move(openedBackup).value();
            if (backup.has_value())
            {
                auto backupMetadata = captureSupportedMetadata(backup->handle.get());
                if (!backupMetadata)
                {
                    return Domain::Result<void>::failure(std::move(backupMetadata).error());
                }
            }
        }

        const std::size_t requiredTemporarySlots = existing && retainBackup ? 2U : 1U;
        auto scavenged = scavengeTemporaryFiles(parentDirectory, requiredTemporarySlots, context);
        if (!scavenged)
        {
            return scavenged;
        }

        std::optional<PendingTemporaryFile> backupTemporary;
        if (existing && retainBackup)
        {
            auto temporary =
                createTemporaryFile(targetPath, parentDirectory, true, *nativeOperations_);
            if (!temporary)
            {
                return Domain::Result<void>::failure(std::move(temporary).error());
            }
            backupTemporary.emplace(std::move(temporary).value());
            auto copied =
                copyAndFlush(target.value()->handle.get(), backupTemporary->handle(), context);
            if (!copied)
            {
                return copied;
            }
        }

        auto temporary =
            createTemporaryFile(targetPath, parentDirectory, existing, *nativeOperations_);
        if (!temporary)
        {
            return Domain::Result<void>::failure(std::move(temporary).error());
        }
        auto written =
            writeBytes(temporary.value().handle(), content, context, "Atomic file write");
        if (!written)
        {
            return written;
        }

        if (!existing)
        {
            auto durableAttributes = applyAndVerifyNewRecordAttributes(temporary.value().handle());
            if (!durableAttributes)
            {
                return durableAttributes;
            }
        }

        nativeOperations_->beforeReplacementVerification();
        auto verifiedAnchors = resolved.value().revalidateDirectoryAnchors();
        if (!verifiedAnchors)
        {
            return verifiedAnchors;
        }
        auto verifiedTarget = verifyCurrentIdentity(
            parentDirectory, targetName,
            existing ? std::optional<AtomicFileIdentity>{target.value()->identity} : std::nullopt);
        if (!verifiedTarget)
        {
            return verifiedTarget;
        }
        if (existing && retainBackup)
        {
            auto verifiedBackup = verifyCurrentIdentity(
                parentDirectory, backupName,
                backup.has_value() ? std::optional<AtomicFileIdentity>{backup->identity}
                                   : std::nullopt);
            if (!verifiedBackup)
            {
                return verifiedBackup;
            }
        }

        if (existing)
        {
            auto finalMetadata = captureSupportedMetadata(target.value()->handle.get());
            if (!finalMetadata)
            {
                return Domain::Result<void>::failure(std::move(finalMetadata).error());
            }
            if (!targetMetadata->equivalentTo(finalMetadata.value()))
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "Atomic target DACL or creation time changed before commit.", true));
            }
            auto applied = applyAndVerifyMetadata(temporary.value().handle(), *targetMetadata);
            if (!applied)
            {
                return applied;
            }
            if (backupTemporary.has_value())
            {
                applied = applyAndVerifyMetadata(backupTemporary->handle(), *targetMetadata);
                if (!applied)
                {
                    return applied;
                }
            }
        }

        validContext = validateOperationContext(context, std::chrono::steady_clock::now(),
                                                "Atomic file commit");
        if (!validContext)
        {
            return validContext;
        }

        if (backupTemporary.has_value())
        {
            auto publishedBackup = publishTemporary(
                *backupTemporary, parentDirectory, backupName, backup.has_value(),
                backup.has_value() ? std::optional<AtomicFileIdentity>{backup->identity}
                                   : std::nullopt,
                context, "Publish atomic recovery backup", *nativeOperations_);
            if (!publishedBackup)
            {
                return publishedBackup;
            }
        }

        if (existing)
        {
            auto commitMetadata = captureSupportedMetadata(target.value()->handle.get());
            if (!commitMetadata)
            {
                return Domain::Result<void>::failure(std::move(commitMetadata).error());
            }
            if (!targetMetadata->equivalentTo(commitMetadata.value()))
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "Atomic target DACL or creation time changed at commit.", true));
            }
        }

        auto publishedTarget = publishTemporary(
            temporary.value(), parentDirectory, targetName, existing,
            existing ? std::optional<AtomicFileIdentity>{target.value()->identity} : std::nullopt,
            context, "Publish atomic replacement", *nativeOperations_);
        if (!publishedTarget)
        {
            return publishedTarget;
        }

        // The handle-relative rename is the linearization point. Cancellation
        // racing after this point returns success because the new complete content
        // is already live.
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Atomic file replacement failed at the Windows boundary."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
