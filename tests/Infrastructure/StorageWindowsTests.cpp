#include "TestSupport.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"
#include "Infrastructure/Windows/Detail/AtomicReplaceEngine.h"
#include "Infrastructure/Windows/Detail/RelativeFileOperations.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UniqueLocalAllocation.h"
#include "Infrastructure/Windows/Detail/UniqueRegistryKey.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests
{
namespace
{

using Infrastructure::Windows::BCryptSha256Hasher;
using Infrastructure::Windows::DpapiSecureStorage;
using Infrastructure::Windows::WindowsAtomicFileStore;
using Infrastructure::Windows::WindowsConfigurationStore;

[[nodiscard]] Domain::OperationContext liveContext(const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("22222222-2222-4222-8222-222222222222"),
        std::chrono::steady_clock::now() + std::chrono::seconds{30}, cancellation,
        parse<Domain::CorrelationId>("p06-storage-test")};
}

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    if (value.empty())
        return {};
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0, "test path must convert to UTF-8");
    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), converted.data(),
                                            required, nullptr, nullptr);
    require(written == required, "test path UTF-8 conversion must be complete");
    return converted;
}

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        require(length > 0U && length < buffer.size(), "GetTempPathW must succeed");
        const std::filesystem::path root{
            std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        for (std::uint64_t attempt = 0; attempt < 32U; ++attempt)
        {
            path_ = root / (L"forge-storage-tests-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error))
            {
                return;
            }
        }
        throw TestFailure{"could not create isolated storage test directory"};
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority
{
  public:
    [[nodiscard]] static Contracts::AuthorizedPath issue(const std::filesystem::path &root,
                                                         const std::filesystem::path &target,
                                                         const Domain::FileAccess access)
    {
        auto authority =
            take(issueAuthority(parse<Domain::AuthorityId>("33333333-3333-4333-8333-333333333333"),
                                parse<Domain::ProjectId>("44444444-4444-4444-8444-444444444444"),
                                parse<Domain::ClientId>("p06-storage-test-client"),
                                {take(Domain::PathText::create(utf8(root.native())))}, access,
                                {access}, {}, false, 1U));
        return take(
            issueAuthorizedPath(authority, take(Domain::PathText::create(utf8(target.native()))),
                                take(Domain::PathText::create(utf8(root.native()))), access));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId &, const Domain::OperationContext &) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "test issuer does not resolve project authority"));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority &, const std::vector<Domain::PathText> &,
        const std::vector<Domain::FileAccess> &, bool, std::uint64_t,
        const Domain::OperationContext &) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "test issuer does not narrow authority"));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority &, const Domain::PathAuthorizationRequest &,
        const Domain::OperationContext &) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "test issuer does not resolve paths"));
    }
};

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result(text.size());
    std::transform(text.begin(), text.end(), result.begin(), [](const char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return result;
}

[[nodiscard]] std::string text(const std::span<const std::byte> content)
{
    return std::string{reinterpret_cast<const char *>(content.data()), content.size()};
}

[[nodiscard]] Infrastructure::Windows::Detail::AtomicNativeCallResult nativeRenameSameDirectory(
    const HANDLE source, const std::wstring_view destinationName,
    const bool replaceExisting) noexcept
{
    try
    {
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                           sizeof(std::uint64_t));
        auto *const information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        std::memset(information, 0, informationBytes);
        information->Flags =
            replaceExisting ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS
                            : 0U;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';

        struct NativeIoStatusBlock final
        {
            union {
                LONG status;
                void *pointer;
            } result{};
            ULONG_PTR information{};
        } ioStatus{};
        using NtSetInformationFileFunction =
            LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG, ULONG);
        using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntSetInformationFile =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<NtSetInformationFileFunction>(
                                   ::GetProcAddress(ntdll, "NtSetInformationFile"));
        const auto rtlNtStatusToDosError =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                                   ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
        {
            return {false, ERROR_PROC_NOT_FOUND};
        }
        constexpr ULONG NativeFileRenameInformationEx = 65U;
        const LONG status = ntSetInformationFile(source, &ioStatus, information,
                                                 static_cast<ULONG>(informationBytes),
                                                 NativeFileRenameInformationEx);
        if (status >= 0)
        {
            return {true, ERROR_SUCCESS};
        }
        return {false, static_cast<std::uint32_t>(rtlNtStatusToDosError(status))};
    }
    catch (...)
    {
        return {false, ERROR_NOT_ENOUGH_MEMORY};
    }
}

[[nodiscard]] Infrastructure::Windows::Detail::AtomicNativeCallResult nativeHardLinkSameDirectory(
    const HANDLE source, const std::wstring_view destinationName) noexcept
{
    try
    {
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                           sizeof(std::uint64_t));
        auto *const information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        std::memset(information, 0, informationBytes);
        information->ReplaceIfExists = FALSE;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';

        struct NativeIoStatusBlock final
        {
            union {
                LONG status;
                void *pointer;
            } result{};
            ULONG_PTR information{};
        } ioStatus{};
        using NtSetInformationFileFunction =
            LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG, ULONG);
        using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntSetInformationFile =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<NtSetInformationFileFunction>(
                                   ::GetProcAddress(ntdll, "NtSetInformationFile"));
        const auto rtlNtStatusToDosError =
            ntdll == nullptr ? nullptr
                             : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                                   ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
        {
            return {false, ERROR_PROC_NOT_FOUND};
        }
        constexpr ULONG NativeFileLinkInformation = 11U;
        const LONG status =
            ntSetInformationFile(source, &ioStatus, information,
                                 static_cast<ULONG>(informationBytes), NativeFileLinkInformation);
        if (status >= 0)
        {
            return {true, ERROR_SUCCESS};
        }
        return {false, static_cast<std::uint32_t>(rtlNtStatusToDosError(status))};
    }
    catch (...)
    {
        return {false, ERROR_NOT_ENOUGH_MEMORY};
    }
}

[[nodiscard]] bool setFixtureExtendedAttribute(const HANDLE file, DWORD &nativeError) noexcept
{
    struct NativeIoStatusBlock final
    {
        union {
            LONG status;
            void *pointer;
        } result{};
        ULONG_PTR information{};
    } ioStatus{};
    struct NativeFullEaInformation final
    {
        ULONG nextEntryOffset{};
        UCHAR flags{};
        UCHAR nameLength{};
        USHORT valueLength{};
        char name[1];
    };
    using NtSetEaFileFunction = LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG);
    using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto ntSetEaFile =
        ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtSetEaFileFunction>(::GetProcAddress(ntdll, "NtSetEaFile"));
    const auto rtlNtStatusToDosError = ntdll == nullptr
                                           ? nullptr
                                           : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                                                 ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntSetEaFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        nativeError = ERROR_PROC_NOT_FOUND;
        return false;
    }

    constexpr std::string_view Name = "forge.test";
    constexpr std::byte Value{0x5a};
    const std::size_t bytes = offsetof(NativeFullEaInformation, name) + Name.size() + 1U + 1U;
    std::vector<std::uint64_t> storage((bytes + sizeof(std::uint64_t) - 1U) /
                                       sizeof(std::uint64_t));
    auto *const information = reinterpret_cast<NativeFullEaInformation *>(storage.data());
    std::memset(information, 0, bytes);
    information->nameLength = static_cast<UCHAR>(Name.size());
    information->valueLength = 1U;
    std::memcpy(information->name, Name.data(), Name.size());
    information->name[Name.size()] = '\0';
    std::memcpy(reinterpret_cast<std::byte *>(information->name) + Name.size() + 1U, &Value, 1U);

    const LONG status = ntSetEaFile(file, &ioStatus, information, static_cast<ULONG>(bytes));
    if (status < 0)
    {
        nativeError = static_cast<DWORD>(rtlNtStatusToDosError(status));
        return false;
    }
    nativeError = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool setFixtureLowMandatoryLabel(const HANDLE file, DWORD &nativeError) noexcept
{
    PSECURITY_DESCRIPTOR rawDescriptor{};
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1,
                                                               &rawDescriptor, nullptr) == FALSE)
    {
        nativeError = ::GetLastError();
        return false;
    }
    Infrastructure::Windows::Detail::UniqueLocalAllocation<void> descriptor{rawDescriptor};
    BOOL present{};
    BOOL defaulted{};
    PACL labelAcl{};
    if (::GetSecurityDescriptorSacl(descriptor.get(), &present, &labelAcl, &defaulted) == FALSE ||
        present == FALSE || labelAcl == nullptr)
    {
        nativeError = ::GetLastError();
        return false;
    }
    nativeError = ::SetSecurityInfo(file, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr,
                                    nullptr, nullptr, labelAcl);
    return nativeError == ERROR_SUCCESS;
}

class ScriptedAtomicNativeOperations final
    : public Infrastructure::Windows::Detail::IAtomicReplaceNativeOperations
{
  public:
    std::vector<std::array<std::byte, 16U>> entropyValues;
    std::function<void()> beforeFirstEntropy;
    std::function<void(std::wstring_view)> beforeReadOpenAction;
    std::function<void()> beforeReplacementVerificationAction;
    std::function<void(std::wstring_view)> beforePublishAction;
    std::wstring failDestinationName;
    DWORD injectedRenameError{ERROR_ACCESS_DENIED};
    bool posixRenameSupported{true};
    bool setupSucceeded{true};
    std::size_t entropyRequests{};
    std::size_t featureRequests{};
    std::size_t renameRequests{};
    std::vector<std::wstring> renameDestinations;

    [[nodiscard]] Domain::Result<std::array<std::byte, 16U>> randomBytes() noexcept override
    {
        try
        {
            if (entropyRequests == 0U && beforeFirstEntropy)
            {
                beforeFirstEntropy();
            }
            ++entropyRequests;
            if (entropyValues.empty())
            {
                std::array<std::byte, 16U> fallback{};
                fallback.fill(std::byte{0x7a});
                return Domain::Result<std::array<std::byte, 16U>>::success(fallback);
            }
            const std::size_t index = (std::min)(entropyRequests - 1U, entropyValues.size() - 1U);
            return Domain::Result<std::array<std::byte, 16U>>::success(entropyValues[index]);
        }
        catch (...)
        {
            return Domain::Result<std::array<std::byte, 16U>>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "The scripted atomic entropy callback failed."));
        }
    }

    void beforeReadOpen(const std::wstring_view leafName) noexcept override
    {
        try
        {
            if (beforeReadOpenAction)
            {
                beforeReadOpenAction(leafName);
            }
        }
        catch (...)
        {
            setupSucceeded = false;
        }
    }

    void beforeReplacementVerification() noexcept override
    {
        try
        {
            if (beforeReplacementVerificationAction)
            {
                beforeReplacementVerificationAction();
            }
        }
        catch (...)
        {
            setupSucceeded = false;
        }
    }

    [[nodiscard]] Infrastructure::Windows::Detail::AtomicFeatureCallResult queryPosixRenameSupport(
        HANDLE) noexcept override
    {
        ++featureRequests;
        return {true, posixRenameSupported, ERROR_SUCCESS};
    }

    void beforePublish(const std::wstring_view destinationName) noexcept override
    {
        try
        {
            if (beforePublishAction)
            {
                beforePublishAction(destinationName);
            }
        }
        catch (...)
        {
            setupSucceeded = false;
        }
    }

    [[nodiscard]] Infrastructure::Windows::Detail::AtomicNativeCallResult renameFile(
        const HANDLE source, const HANDLE destinationDirectory,
        const std::wstring_view destinationName, const bool replaceExisting) noexcept override
    {
        try
        {
            static_cast<void>(destinationDirectory);
            ++renameRequests;
            renameDestinations.emplace_back(destinationName);
            if (!failDestinationName.empty() &&
                CompareStringOrdinal(failDestinationName.data(),
                                     static_cast<int>(failDestinationName.size()),
                                     destinationName.data(),
                                     static_cast<int>(destinationName.size()), TRUE) == CSTR_EQUAL)
            {
                return {false, injectedRenameError};
            }

            return nativeRenameSameDirectory(source, destinationName, replaceExisting);
        }
        catch (...)
        {
            setupSucceeded = false;
            return {false, ERROR_NOT_ENOUGH_MEMORY};
        }
    }
};

[[nodiscard]] std::array<std::byte, 16U> filledEntropy(const unsigned char value)
{
    std::array<std::byte, 16U> result{};
    result.fill(static_cast<std::byte>(value));
    return result;
}

[[nodiscard]] std::filesystem::path temporaryCandidate(const std::filesystem::path &parent,
                                                       const std::array<std::byte, 16U> &entropy)
{
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring name{L".forge-tmp-"};
    for (const std::byte item : entropy)
    {
        const auto value = std::to_integer<unsigned int>(item);
        name.push_back(Hex[(value >> 4U) & 0x0fU]);
        name.push_back(Hex[value & 0x0fU]);
    }
    name.append(L".tmp");
    return parent / name;
}

constexpr wchar_t AtomicCrashChildVariable[] = L"FORGE_ATOMIC_CRASH_CHILD";
constexpr wchar_t AtomicCrashRootVariable[] = L"FORGE_ATOMIC_CRASH_ROOT";
constexpr wchar_t AtomicCrashReadyEventVariable[] = L"FORGE_ATOMIC_CRASH_READY_EVENT";
constexpr DWORD AtomicCrashTerminationCode = 0xF06C0001U;

[[nodiscard]] std::wstring atomicCrashEnvironmentValue(const wchar_t *const name)
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U)
    {
        require(::GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                "atomic crash-child environment value must not be empty");
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(written > 0U && written < buffer.size(),
            "atomic crash-child environment value must be bounded");
    return std::wstring{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] bool atomicCrashChildRequested()
{
    return atomicCrashEnvironmentValue(AtomicCrashChildVariable) == L"1";
}

class ScopedAtomicCrashEnvironment final
{
  public:
    ScopedAtomicCrashEnvironment(const wchar_t *const name, const std::wstring_view value)
        : name_{name}
    {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD required = ::GetEnvironmentVariableW(name_.c_str(), nullptr, 0U);
        if (required != 0U)
        {
            std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
            const DWORD written = ::GetEnvironmentVariableW(
                name_.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
            require(written < buffer.size(),
                    "atomic crash environment fixture must save the existing value");
            previous_.emplace(buffer.data(), static_cast<std::size_t>(written));
        }
        else
        {
            require(::GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                    "atomic crash environment fixture must read the existing value");
        }
        require(::SetEnvironmentVariableW(name_.c_str(), std::wstring{value}.c_str()) != FALSE,
                "atomic crash environment fixture must set its child value");
    }

    ~ScopedAtomicCrashEnvironment() noexcept
    {
        static_cast<void>(::SetEnvironmentVariableW(
            name_.c_str(), previous_.has_value() ? previous_->c_str() : nullptr));
    }

    ScopedAtomicCrashEnvironment(const ScopedAtomicCrashEnvironment &) = delete;
    ScopedAtomicCrashEnvironment &operator=(const ScopedAtomicCrashEnvironment &) = delete;
    ScopedAtomicCrashEnvironment(ScopedAtomicCrashEnvironment &&) = delete;
    ScopedAtomicCrashEnvironment &operator=(ScopedAtomicCrashEnvironment &&) = delete;

  private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

class AtomicCrashChildProcess final
{
  public:
    explicit AtomicCrashChildProcess(const PROCESS_INFORMATION &information) noexcept
        : process_{information.hProcess}, thread_{information.hThread}
    {
    }

    ~AtomicCrashChildProcess() noexcept
    {
        if (process_)
        {
            DWORD exitCode{};
            if (::GetExitCodeProcess(process_.get(), &exitCode) != FALSE &&
                exitCode == STILL_ACTIVE)
            {
                static_cast<void>(::TerminateProcess(process_.get(), AtomicCrashTerminationCode));
                static_cast<void>(::WaitForSingleObject(process_.get(), 5'000U));
            }
        }
    }

    AtomicCrashChildProcess(const AtomicCrashChildProcess &) = delete;
    AtomicCrashChildProcess &operator=(const AtomicCrashChildProcess &) = delete;
    AtomicCrashChildProcess(AtomicCrashChildProcess &&) = delete;
    AtomicCrashChildProcess &operator=(AtomicCrashChildProcess &&) = delete;

    [[nodiscard]] HANDLE process() const noexcept
    {
        return process_.get();
    }

    void terminateAndClose()
    {
        require(::TerminateProcess(process_.get(), AtomicCrashTerminationCode) != FALSE,
                "atomic crash child must terminate with its test sentinel");
        require(::WaitForSingleObject(process_.get(), 5'000U) == WAIT_OBJECT_0,
                "atomic crash child termination must drain within five seconds");
        DWORD exitCode{};
        require(::GetExitCodeProcess(process_.get(), &exitCode) != FALSE &&
                    exitCode == AtomicCrashTerminationCode,
                "atomic crash child must report the forced-termination sentinel");
        thread_.reset();
        process_.reset();
    }

  private:
    Infrastructure::Windows::Detail::UniqueHandle process_;
    Infrastructure::Windows::Detail::UniqueHandle thread_;
};

[[nodiscard]] Infrastructure::Windows::Detail::UniqueHandle openAtomicCrashParent(
    const std::filesystem::path &path)
{
    Infrastructure::Windows::Detail::UniqueHandle parent{::CreateFileW(
        path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | FILE_ADD_FILE |
                          FILE_DELETE_CHILD,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(parent),
            "atomic crash fixture must retain its explicit staging parent handle");
    return parent;
}

[[nodiscard]] Infrastructure::Windows::Detail::UniqueHandle createAtomicCrashStage(
    const HANDLE parent, const std::filesystem::path &path, const std::string_view content,
    const bool flush)
{
    Infrastructure::Windows::Detail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                            FILE_READ_EA | DELETE;
    options.shareAccess = 0U;
    options.disposition = Infrastructure::Windows::Detail::RelativeOpenDisposition::CreateNew;
    options.fileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    options.objectType = Infrastructure::Windows::Detail::RelativeObjectType::File;
    options.writeThrough = true;
    auto opened = Infrastructure::Windows::Detail::openRelative(
        parent, path.filename().wstring(), options);
    Infrastructure::Windows::Detail::UniqueHandle stage{std::move(opened.handle)};
    require(static_cast<bool>(stage),
            "atomic crash child must create an exclusive production-shaped stage");
    DWORD written{};
    require(::WriteFile(stage.get(), content.data(), static_cast<DWORD>(content.size()), &written,
                        nullptr) != FALSE &&
                written == content.size(),
            "atomic crash child must write its staged bytes");
    if (flush)
    {
        require(::FlushFileBuffers(stage.get()) != FALSE,
                "atomic crash child must reach the post-flush checkpoint");
        FILE_BASIC_INFO durable{};
        durable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        require(::SetFileInformationByHandle(stage.get(), FileBasicInfo, &durable,
                                             sizeof(durable)) != FALSE,
                "atomic crash child must apply durable post-flush attributes");
    }
    return stage;
}

void atomicCrashRecoveryChild()
{
    const auto root = std::filesystem::path{atomicCrashEnvironmentValue(AtomicCrashRootVariable)};
    const auto readyName = atomicCrashEnvironmentValue(AtomicCrashReadyEventVariable);
    require(!root.empty() && std::filesystem::is_directory(root),
            "atomic crash child must receive an explicit existing root");
    require(!readyName.empty(), "atomic crash child must receive a readiness event name");

    auto parent = openAtomicCrashParent(root);
    auto preFlush = createAtomicCrashStage(
        parent.get(), temporaryCandidate(root, filledEntropy(0xc1U)), "pre-flush-stage", false);
    auto postFlush = createAtomicCrashStage(
        parent.get(), temporaryCandidate(root, filledEntropy(0xc2U)), "post-flush-stage", true);
    Infrastructure::Windows::Detail::UniqueHandle ready{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str())};
    require(static_cast<bool>(ready), "atomic crash child must open its readiness event");
    require(::SetEvent(ready.get()) != FALSE,
            "atomic crash child must signal only after retaining both no-share stage handles");

    ::Sleep(INFINITE);
    throw TestFailure{"atomic crash child unexpectedly resumed after its readiness signal"};
}

void createEmptyFixtureFile(const std::filesystem::path &path,
                            const DWORD attributes = FILE_ATTRIBUTE_NORMAL)
{
    Infrastructure::Windows::Detail::UniqueHandle handle{
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW, attributes, nullptr)};
    require(static_cast<bool>(handle), "could not create an atomic collision fixture");
}

void requireNoTemporaryAttribute(const std::filesystem::path &path, const std::string_view message)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_TEMPORARY) == 0U,
            message);
}

void requireNoAtomicTemporaryFiles(const std::filesystem::path &directory)
{
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        require(!entry.path().filename().wstring().starts_with(L".forge-tmp-"),
                "atomic operation leaked a temporary file");
    }
}

struct TestFileIdentity final
{
    std::uint64_t volumeSerialNumber{};
    std::array<std::byte, 16U> fileId{};

    bool operator==(const TestFileIdentity &) const = default;
};

[[nodiscard]] TestFileIdentity fileIdentity(const HANDLE file)
{
    FILE_ID_INFO information{};
    require(::GetFileInformationByHandleEx(file, FileIdInfo, &information, sizeof(information)) !=
                FALSE,
            "test fixture must read a file identity");
    TestFileIdentity identity{};
    identity.volumeSerialNumber = information.VolumeSerialNumber;
    std::memcpy(identity.fileId.data(), information.FileId.Identifier, identity.fileId.size());
    return identity;
}

[[nodiscard]] std::string readHandleText(const HANDLE file)
{
    LARGE_INTEGER beginning{};
    require(::SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) != FALSE,
            "test fixture must rewind an open file");
    std::array<char, 256U> buffer{};
    DWORD count{};
    require(::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
                FALSE,
            "test fixture must read an open file");
    return std::string{buffer.data(), count};
}

struct TestDaclSnapshot final
{
    std::vector<std::byte> bytes;
    bool nullDacl{};
    bool protectedDacl{};

    bool operator==(const TestDaclSnapshot &) const = default;
};

[[nodiscard]] TestDaclSnapshot daclSnapshot(const HANDLE file)
{
    PSECURITY_DESCRIPTOR rawDescriptor{};
    const DWORD error = ::GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                          nullptr, nullptr, nullptr, &rawDescriptor);
    Infrastructure::Windows::Detail::UniqueLocalAllocation<void> descriptor{rawDescriptor};
    require(error == ERROR_SUCCESS && static_cast<bool>(descriptor),
            "test fixture must read a file DACL");
    BOOL present{};
    BOOL defaulted{};
    PACL dacl{};
    require(::GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted) != FALSE &&
                present != FALSE,
            "test fixture DACL must be present");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    require(::GetSecurityDescriptorControl(descriptor.get(), &control, &revision) != FALSE,
            "test fixture must read DACL control");
    TestDaclSnapshot snapshot{};
    snapshot.nullDacl = dacl == nullptr;
    snapshot.protectedDacl = (control & SE_DACL_PROTECTED) != 0U;
    if (dacl != nullptr)
    {
        require(::IsValidAcl(dacl) != FALSE, "test fixture DACL must be valid");
        snapshot.bytes.resize(dacl->AclSize);
        std::memcpy(snapshot.bytes.data(), dacl, snapshot.bytes.size());
    }
    return snapshot;
}

[[nodiscard]] std::int64_t creationTime(const HANDLE file)
{
    FILE_BASIC_INFO information{};
    require(::GetFileInformationByHandleEx(file, FileBasicInfo, &information,
                                           sizeof(information)) != FALSE,
            "test fixture must read file basic information");
    return information.CreationTime.QuadPart;
}

void setCreationTime(const HANDLE file, const std::int64_t value)
{
    FILE_BASIC_INFO information{};
    information.CreationTime.QuadPart = value;
    require(::SetFileInformationByHandle(file, FileBasicInfo, &information, sizeof(information)) !=
                FALSE,
            "test fixture must set a deterministic creation time");
}

void createDirectoryJunction(const std::filesystem::path &junction,
                             const std::filesystem::path &destination)
{
    require(std::filesystem::create_directory(junction),
            "could not create an atomic junction fixture directory");
    Infrastructure::Windows::Detail::UniqueHandle handle{
        ::CreateFileW(junction.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(handle), "could not open an atomic junction fixture directory");

    const std::wstring substitute = L"\\??\\" + destination.native();
    const std::wstring printName = destination.native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);

    struct MountPointReparseData final
    {
        ULONG tag;
        USHORT dataLength;
        USHORT reserved;
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
        wchar_t pathBuffer[1];
    };

    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t bufferBytes = offsetof(MountPointReparseData, pathBuffer) + pathBytes;
    require(bufferBytes <= (std::numeric_limits<DWORD>::max)(),
            "atomic junction fixture exceeded the DeviceIoControl bound");
    std::vector<std::uint64_t> storage((bufferBytes + sizeof(std::uint64_t) - 1U) /
                                       sizeof(std::uint64_t));
    auto *const data = reinterpret_cast<MountPointReparseData *>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<USHORT>(bufferBytes - 8U);
    data->reserved = 0U;
    data->substituteOffset = 0U;
    data->substituteLength = static_cast<USHORT>(substituteBytes);
    data->printOffset = static_cast<USHORT>(substituteBytes + sizeof(wchar_t));
    data->printLength = static_cast<USHORT>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    data->pathBuffer[substitute.size()] = L'\0';
    std::memcpy(reinterpret_cast<std::byte *>(data->pathBuffer) + data->printOffset,
                printName.data(), printBytes);
    data->pathBuffer[(data->printOffset / sizeof(wchar_t)) + printName.size()] = L'\0';

    DWORD returned{};
    require(::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, data,
                              static_cast<DWORD>(bufferBytes), nullptr, 0U, &returned,
                              nullptr) != FALSE,
            "could not create an atomic directory junction fixture");
}

[[nodiscard]] std::wstring fixtureStoredValueName(const std::uint64_t suffix)
{
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring name{L"v1_"};
    name.append(64U, L'0');
    auto remaining = suffix;
    for (std::size_t offset = 0U; offset < 16U; ++offset)
    {
        name[name.size() - 1U - offset] = Hex[static_cast<std::size_t>(remaining & 0xFU)];
        remaining >>= 4U;
    }
    return name;
}

[[nodiscard]] std::vector<BYTE> fixtureStoredBlob()
{
    return {0x46U, 0x43U, 0x44U, 0x50U, 0x01U, 0x00U};
}
class MemoryAtomicFileStore final : public Contracts::IAtomicFileStore
{
  public:
    std::vector<std::byte> content;
    std::vector<std::byte> backupContent;
    bool exists{};
    bool backupExists{};
    bool failReplace{};
    bool lastRetainBackup{};
    std::optional<Domain::FileAccess> lastAccess;
    std::stop_source *cancelAfterCommit{};

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath &path, const std::size_t maximumBytes,
        const Domain::OperationContext &) noexcept override
    {
        const bool backup = path.canonicalPath().value().ends_with(".bak");
        const bool selectedExists = backup ? backupExists : exists;
        const auto &selectedContent = backup ? backupContent : content;
        if (!selectedExists)
        {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::RecordNotFound, "memory file is missing"));
        }
        if (selectedContent.size() > maximumBytes)
        {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "memory file exceeds requested bound"));
        }
        return Domain::Result<std::vector<std::byte>>::success(selectedContent);
    }

    [[nodiscard]] Domain::Result<void> replace(const Contracts::AuthorizedPath &path,
                                               const std::span<const std::byte> replacement,
                                               const bool retainBackup,
                                               const Domain::OperationContext &) noexcept override
    {
        lastAccess = path.access();
        if (exists && path.access() == Domain::FileAccess::Create)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "memory create cannot overwrite"));
        }
        if (!exists && path.access() == Domain::FileAccess::Write)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound, "memory write cannot create"));
        }
        if (failReplace)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::StorageFull, "scripted durable commit failure"));
        }
        if (exists && retainBackup)
        {
            backupContent = content;
            backupExists = true;
        }
        content.assign(replacement.begin(), replacement.end());
        exists = true;
        lastRetainBackup = retainBackup;
        if (cancelAfterCommit != nullptr)
        {
            cancelAfterCommit->request_stop();
        }
        return Domain::Result<void>::success();
    }
};

struct ConfigurationFixture final
{
    TemporaryDirectory directory;
    std::filesystem::path configPath{directory.path() / L"config.json"};
    Contracts::AuthorizedPath readPath{
        CapabilityIssuer::issue(directory.path(), configPath, Domain::FileAccess::Read)};
    Contracts::AuthorizedPath writePath{
        CapabilityIssuer::issue(directory.path(), configPath, Domain::FileAccess::Write)};
    Contracts::AuthorizedPath createPath{
        CapabilityIssuer::issue(directory.path(), configPath, Domain::FileAccess::Create)};
    Contracts::AuthorizedPath backupReadPath{CapabilityIssuer::issue(
        directory.path(), configPath.wstring() + L".bak", Domain::FileAccess::Read)};
};

class RegistryScope final
{
  public:
    RegistryScope()
        : subkey_{L"Software\\Forge Conductor\\Tests\\Storage-" +
                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(GetTickCount64()) +
                  L"-" + std::to_wstring(nextSequence())}
    {
    }

    RegistryScope(const RegistryScope &) = delete;
    RegistryScope &operator=(const RegistryScope &) = delete;
    ~RegistryScope()
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, subkey_.c_str());
    }

    [[nodiscard]] const std::wstring &subkey() const noexcept
    {
        return subkey_;
    }

  private:
    [[nodiscard]] static std::uint64_t nextSequence() noexcept
    {
        static std::atomic<std::uint64_t> sequence{};
        return sequence.fetch_add(1U, std::memory_order_relaxed);
    }

    std::wstring subkey_;
};
class ScopedAnonymousImpersonation final
{
  public:
    ScopedAnonymousImpersonation()
    {
        if (!ImpersonateAnonymousToken(GetCurrentThread()))
        {
            throw TestFailure{"could not impersonate the anonymous Windows SID"};
        }
        active_ = true;
    }

    ScopedAnonymousImpersonation(const ScopedAnonymousImpersonation &) = delete;
    ScopedAnonymousImpersonation &operator=(const ScopedAnonymousImpersonation &) = delete;

    ~ScopedAnonymousImpersonation()
    {
        if (active_)
        {
            static_cast<void>(RevertToSelf());
        }
    }

    [[nodiscard]] bool revert() noexcept
    {
        if (!active_)
            return true;
        if (!RevertToSelf())
            return false;
        active_ = false;
        return true;
    }

  private:
    bool active_{};
};

[[nodiscard]] Infrastructure::Windows::Detail::UniqueLocalAllocation<std::byte>
tokenUserInformation(const HANDLE token)
{
    DWORD required{};
    SetLastError(ERROR_SUCCESS);
    const BOOL sized = GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    require(!sized && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            "could not size Windows token-user information");
    require(required >= sizeof(TOKEN_USER) && required <= 4096U,
            "Windows token-user information exceeded its test bound");

    auto *raw = static_cast<std::byte *>(LocalAlloc(LPTR, required));
    require(raw != nullptr, "could not allocate token-user information");
    Infrastructure::Windows::Detail::UniqueLocalAllocation<std::byte> buffer{raw};
    require(GetTokenInformation(token, TokenUser, buffer.get(), required, &required) != FALSE,
            "could not read Windows token-user information");
    return buffer;
}

[[nodiscard]] std::string sidText(const PSID sid)
{
    require(IsValidSid(sid) != FALSE, "Windows returned an invalid user SID");
    LPSTR raw{};
    require(ConvertSidToStringSidA(sid, &raw) != FALSE, "could not format a Windows user SID");
    Infrastructure::Windows::Detail::UniqueLocalAllocation<char> textOwner{raw};
    return std::string{raw};
}

[[nodiscard]] std::byte hexByte(const char high, const char low)
{
    const auto nibble = [](const char value) -> unsigned int {
        if (value >= '0' && value <= '9')
        {
            return static_cast<unsigned int>(value - '0');
        }
        if (value >= 'a' && value <= 'f')
        {
            return 10U + static_cast<unsigned int>(value - 'a');
        }
        throw TestFailure{"SHA-256 digest contained a non-hex character"};
    };
    return static_cast<std::byte>((nibble(high) << 4U) | nibble(low));
}

void atomicReplaceRoundTripBackupAndBounds()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"state.bin";
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    WindowsAtomicFileStore store;

    const auto first = bytes("first");
    const auto created = store.replace(createPath, first, false, liveContext());
    require(static_cast<bool>(created), created
                                            ? "atomic create must succeed"
                                            : "atomic create failed with " + created.error().code +
                                                  ": " + created.error().message);
    const auto loadedFirst = take(store.read(readPath, first.size(), liveContext()));
    require(loadedFirst == first, "atomic create must round-trip exact bytes");
    requireNoTemporaryAttribute(target, "a newly published atomic record retained TEMPORARY");

    const auto second = bytes("second");
    requireError(store.replace(createPath, second, true, liveContext()),
                 Domain::ErrorCodes::Unauthorized,
                 "create authority must not overwrite an existing file");
    const auto replaced = store.replace(writePath, second, true, liveContext());
    require(static_cast<bool>(replaced), replaced ? "atomic replacement with backup must succeed"
                                                  : "atomic replacement failed with " +
                                                        replaced.error().code + ": " +
                                                        replaced.error().message);
    require(take(store.read(readPath, second.size(), liveContext())) == second,
            "atomic replacement must publish the new bytes");
    requireNoTemporaryAttribute(target, "an atomic replacement retained TEMPORARY");

    const auto backup = target.wstring() + L".bak";
    auto backupRead = CapabilityIssuer::issue(directory.path(), backup, Domain::FileAccess::Read);
    require(take(store.read(backupRead, first.size(), liveContext())) == first,
            "atomic replacement must retain the prior file as .bak");
    requireNoTemporaryAttribute(backup, "an atomic recovery backup retained TEMPORARY");

    const auto excessiveRead =
        store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes + 1U, liveContext());
    requireError(excessiveRead, Domain::ErrorCodes::LimitExceeded,
                 "atomic read cap+1 must fail with limit_exceeded");

    std::vector<std::byte> excessiveContent(WindowsAtomicFileStore::MaximumContentBytes + 1U);
    const auto excessiveReplace = store.replace(createPath, excessiveContent, false, liveContext());
    requireError(excessiveReplace, Domain::ErrorCodes::PayloadTooLarge,
                 "atomic replace cap+1 must fail with payload_too_large");

    for (const auto &entry : std::filesystem::directory_iterator(directory.path()))
    {
        require(!entry.path().filename().wstring().starts_with(L".forge-tmp-"),
                "atomic replacement must not leak temporary files");
    }
}

void atomicReplaceRejectsMissingWriteAdsAndCancellation()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"missing.bin";
    WindowsAtomicFileStore store;
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    const auto missing = store.replace(writePath, bytes("x"), false, liveContext());
    requireError(missing, Domain::ErrorCodes::RecordNotFound,
                 "write authority must not create a missing file");

    auto adsPath = CapabilityIssuer::issue(directory.path(), target.wstring() + L":hidden",
                                           Domain::FileAccess::Create);
    const auto ads = store.replace(adsPath, bytes("x"), false, liveContext());
    requireError(ads, Domain::ErrorCodes::InvalidRequest,
                 "atomic replacement must reject alternate data streams");

    std::stop_source cancellation;
    cancellation.request_stop();
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    const auto cancelled =
        store.replace(createPath, bytes("x"), false, liveContext(cancellation.get_token()));
    requireError(cancelled, Domain::ErrorCodes::Cancelled,
                 "pre-cancelled atomic replacement must fail closed");
    require(!std::filesystem::exists(target),
            "pre-cancelled atomic replacement must not create a file");
}

void atomicReplacePinsParentAncestry()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / L"owned";
    require(std::filesystem::create_directory(parent),
            "atomic anchor fixture parent must be created");
    const auto renamedParent = directory.path() / L"renamed-owned";
    const auto target = parent / L"state.bin";

    {
        Infrastructure::Windows::Detail::UniqueHandle preAnchorWrite{::CreateFileW(
            parent.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        require(static_cast<bool>(preAnchorWrite),
                "atomic anchor fixture must allow a directory write handle before "
                "anchoring");
    }
    require(::MoveFileExW(parent.c_str(), renamedParent.c_str(), 0U) != FALSE,
            "atomic anchor fixture parent must be renameable before anchoring");
    require(::MoveFileExW(renamedParent.c_str(), parent.c_str(), 0U) != FALSE,
            "atomic anchor fixture parent rename must be restorable before "
            "anchoring");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {filledEntropy(0x31U)};
    bool callbackObserved{};
    bool renameBlocked{};
    bool unexpectedRenameRestored{true};
    DWORD renameError{ERROR_SUCCESS};
    bool reparseWriterBlocked{};
    DWORD reparseWriterError{ERROR_SUCCESS};
    native.beforeFirstEntropy = [&]() noexcept {
        callbackObserved = true;
        Infrastructure::Windows::Detail::UniqueHandle reparseWriter{::CreateFileW(
            parent.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        reparseWriterBlocked = !reparseWriter;
        if (!reparseWriter)
        {
            reparseWriterError = ::GetLastError();
        }
        if (::MoveFileExW(parent.c_str(), renamedParent.c_str(), 0U) == FALSE)
        {
            renameBlocked = true;
            renameError = ::GetLastError();
            return;
        }
        unexpectedRenameRestored =
            ::MoveFileExW(renamedParent.c_str(), parent.c_str(), 0U) != FALSE;
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    require(static_cast<bool>(engine.replace(createPath, bytes("anchored"), false, liveContext())),
            "anchored atomic create must retain legitimate behavior");
    require(callbackObserved, "atomic anchor regression callback did not execute");
    require(reparseWriterBlocked,
            "atomic target parent accepted a write-capable in-place reparse handle");
    require(reparseWriterError == ERROR_SHARING_VIOLATION ||
                reparseWriterError == ERROR_ACCESS_DENIED,
            "atomic target parent rejected reparse writes for an unrelated reason");
    require(renameBlocked, "atomic target parent was renameable after authority validation");
    require(renameError == ERROR_SHARING_VIOLATION || renameError == ERROR_ACCESS_DENIED,
            "atomic target parent rename failed for an unrelated reason");
    require(unexpectedRenameRestored,
            "unexpected parent rename could not be restored by the fixture");

    WindowsAtomicFileStore reader;
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(reader.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                  liveContext()))) == "anchored",
            "anchored atomic create changed the committed bytes");
    requireNoAtomicTemporaryFiles(parent);
}

void atomicReadPinsParentAncestryAndUsesRelativeLeafOpen()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / L"read-owned";
    require(std::filesystem::create_directory(parent),
            "atomic read-anchor fixture parent must be created");
    const auto renamedParent = directory.path() / L"read-renamed-owned";
    const auto target = parent / L"state.bin";

    WindowsAtomicFileStore store;
    auto createPath = CapabilityIssuer::issue(parent, target, Domain::FileAccess::Create);
    require(static_cast<bool>(
                store.replace(createPath, bytes("authorized-read"), false, liveContext())),
            "atomic read-anchor fixture must create its target");
    require(::MoveFileExW(parent.c_str(), renamedParent.c_str(), 0U) != FALSE,
            "read-anchor positive control parent must be renameable before "
            "anchoring");
    require(::MoveFileExW(renamedParent.c_str(), parent.c_str(), 0U) != FALSE,
            "read-anchor positive control parent rename must be restorable");

    ScriptedAtomicNativeOperations native;
    bool callbackObserved{};
    bool renameBlocked{};
    DWORD renameError{ERROR_SUCCESS};
    bool reparseWriterBlocked{};
    DWORD reparseWriterError{ERROR_SUCCESS};
    native.beforeReadOpenAction = [&](const std::wstring_view leafName) noexcept {
        callbackObserved = leafName == target.filename().wstring();
        Infrastructure::Windows::Detail::UniqueHandle reparseWriter{::CreateFileW(
            parent.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        reparseWriterBlocked = !reparseWriter;
        if (!reparseWriter)
        {
            reparseWriterError = ::GetLastError();
        }
        renameBlocked = ::MoveFileExW(parent.c_str(), renamedParent.c_str(), 0U) == FALSE;
        if (renameBlocked)
        {
            renameError = ::GetLastError();
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    auto readPath = CapabilityIssuer::issue(parent, target, Domain::FileAccess::Read);
    const auto loaded =
        engine.read(readPath, WindowsAtomicFileStore::MaximumContentBytes, liveContext());
    require(static_cast<bool>(loaded), loaded ? "anchored relative read must succeed"
                                              : "anchored relative read failed with " +
                                                    loaded.error().code + ": " +
                                                    loaded.error().message);
    require(text(loaded.value()) == "authorized-read",
            "anchored relative read returned bytes from a substituted parent");
    require(callbackObserved, "atomic read pre-open boundary hook did not execute");
    require(reparseWriterBlocked && (reparseWriterError == ERROR_SHARING_VIOLATION ||
                                     reparseWriterError == ERROR_ACCESS_DENIED),
            "anchored read permitted an in-place parent reparse writer");
    require(renameBlocked &&
                (renameError == ERROR_SHARING_VIOLATION || renameError == ERROR_ACCESS_DENIED),
            "anchored read permitted parent substitution before the leaf open");
}

void atomicRejectsCaseSensitiveDirectoryAuthority()
{
    auto insensitive =
        Infrastructure::Windows::Detail::WindowsPathResolver::validateDirectoryCaseSensitivityFlags(
            0U);
    require(static_cast<bool>(insensitive),
            "case-insensitive directory policy positive control must succeed");
    auto injectedSensitive =
        Infrastructure::Windows::Detail::WindowsPathResolver::validateDirectoryCaseSensitivityFlags(
            FILE_CS_FLAG_CASE_SENSITIVE_DIR);
    requireError(injectedSensitive, Domain::ErrorCodes::PathOutsideAuthority,
                 "case-sensitive directory flags must be rejected by the shared "
                 "policy seam");

    TemporaryDirectory directory;
    const auto sensitiveRoot = directory.path() / L"case-sensitive";
    require(std::filesystem::create_directory(sensitiveRoot),
            "case-sensitive policy fixture must create its directory");
    Infrastructure::Windows::Detail::UniqueHandle directoryHandle{::CreateFileW(
        sensitiveRoot.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(directoryHandle),
            "case-sensitive policy fixture must open its directory");

    FILE_CASE_SENSITIVE_INFO caseSensitivity{};
    caseSensitivity.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    const bool enabled =
        ::SetFileInformationByHandle(directoryHandle.get(), FileCaseSensitiveInfo, &caseSensitivity,
                                     sizeof(caseSensitivity)) != FALSE;
    const DWORD capabilityError = enabled ? ERROR_SUCCESS : ::GetLastError();
    std::cout << "[EVIDENCE] atomic_case_sensitive_directory_supported=" << (enabled ? 1 : 0)
              << " native_error=" << capabilityError << '\n';
    if (!enabled)
    {
        return;
    }

    const auto lowerPath = sensitiveRoot / L"state.bin";
    const auto upperPath = sensitiveRoot / L"STATE.BIN";
    constexpr DWORD CaseFixtureFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS |
                                       FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT;
    Infrastructure::Windows::Detail::UniqueHandle lower{
        ::CreateFileW(lowerPath.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | DELETE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                      CaseFixtureFlags, nullptr)};
    Infrastructure::Windows::Detail::UniqueHandle upper{
        ::CreateFileW(upperPath.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | DELETE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                      CaseFixtureFlags, nullptr)};
    require(static_cast<bool>(lower) && static_cast<bool>(upper),
            "enabled case-sensitive directory must admit a case-twin fixture");

    WindowsAtomicFileStore store;
    auto readPath = CapabilityIssuer::issue(sensitiveRoot, lowerPath, Domain::FileAccess::Read);
    requireError(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes, liveContext()),
                 Domain::ErrorCodes::PathOutsideAuthority,
                 "atomic access must reject a case-sensitive authority before "
                 "leaf resolution");

    lower.reset();
    upper.reset();
    caseSensitivity.Flags = 0U;
    require(::SetFileInformationByHandle(directoryHandle.get(), FileCaseSensitiveInfo,
                                         &caseSensitivity, sizeof(caseSensitivity)) != FALSE,
            "case-sensitive policy fixture must restore its directory flag");
}

void atomicReplacementKeepsOldHandleAndPublishesNewIdentity()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"identity-target.bin";

    WindowsAtomicFileStore store;
    auto targetCreate =
        CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    require(static_cast<bool>(
                store.replace(targetCreate, bytes("authorized-original"), false, liveContext())),
            "identity fixture must create the authorized target");

    Infrastructure::Windows::Detail::UniqueHandle oldHandle{::CreateFileW(
        target.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(oldHandle), "identity fixture must retain the old target handle");
    const auto oldIdentity = fileIdentity(oldHandle.get());

    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    const auto replaced = store.replace(writePath, bytes("published-new"), false, liveContext());
    require(static_cast<bool>(replaced),
            replaced ? "handle-relative replacement must succeed with an old read handle"
                     : "handle-relative replacement failed with " + replaced.error().code + ": " +
                           replaced.error().message);
    require(readHandleText(oldHandle.get()) == "authorized-original",
            "the old handle must continue to expose the replaced file bytes");

    Infrastructure::Windows::Detail::UniqueHandle currentHandle{::CreateFileW(
        target.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(currentHandle), "the published target name must reopen");
    require(fileIdentity(currentHandle.get()) != oldIdentity,
            "a new open of the target name must resolve to the staged file identity");
    require(readHandleText(currentHandle.get()) == "published-new",
            "a new open of the target name must expose the replacement bytes");

    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "published-new",
            "store reads must resolve the newly published identity");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementReturnsRetryableConflictForNonDeleteSharingReader()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"reader-boundary.bin";

    WindowsAtomicFileStore store;
    auto targetCreate =
        CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    require(
        static_cast<bool>(store.replace(targetCreate, bytes("stable-old"), false, liveContext())),
        "reader-boundary fixture must create the target");

    Infrastructure::Windows::Detail::UniqueHandle externalReader{::CreateFileW(
        target.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(externalReader),
            "reader-boundary fixture must retain a reader without delete sharing");

    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    const auto rejected = store.replace(writePath, bytes("must-not-publish"), false, liveContext());
    requireError(rejected, Domain::ErrorCodes::Conflict,
                 "a reader without delete sharing must reject handle-relative "
                 "replacement");
    require(rejected.error().retryable,
            "a sharing-conflict replacement must explicitly be retryable");
    require(readHandleText(externalReader.get()) == "stable-old",
            "the conflicting reader must retain the old target bytes");

    externalReader.reset();
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "stable-old",
            "a reader sharing conflict must preserve the target name and old bytes");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementContainsFinalLeafSwapAndTempTamper()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"publish-race-target.bin";
    const auto substitute = directory.path() / L"publish-race-substitute.bin";
    const auto entropy = filledEntropy(0x37U);
    const auto stagedPath = temporaryCandidate(directory.path(), entropy);

    WindowsAtomicFileStore store;
    auto targetCreate =
        CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto substituteCreate =
        CapabilityIssuer::issue(directory.path(), substitute, Domain::FileAccess::Create);
    require(
        static_cast<bool>(store.replace(targetCreate, bytes("old-target"), false, liveContext())),
        "final-swap fixture must create the target");
    require(static_cast<bool>(
                store.replace(substituteCreate, bytes("attacker-bytes"), false, liveContext())),
            "final-swap fixture must create the substitute");

    createEmptyFixtureFile(stagedPath);
    {
        Infrastructure::Windows::Detail::UniqueHandle positiveWriter{
            ::CreateFileW(stagedPath.c_str(), FILE_WRITE_DATA | DELETE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(positiveWriter),
                "staged-tamper positive control must permit a writer before staging");
    }
    require(::DeleteFileW(stagedPath.c_str()) != FALSE,
            "staged-tamper positive control must remove its fixture");

    Infrastructure::Windows::Detail::UniqueHandle attackerHandle{::CreateFileW(
        substitute.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(attackerHandle),
            "final-swap fixture must retain its attacker-controlled source handle");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {entropy};
    bool stagedWriterBlocked{};
    DWORD stagedWriterError{ERROR_SUCCESS};
    bool stagedDeleteBlocked{};
    DWORD stagedDeleteError{ERROR_SUCCESS};
    native.beforeReplacementVerificationAction = [&]() noexcept {
        Infrastructure::Windows::Detail::UniqueHandle stagedWriter{
            ::CreateFileW(stagedPath.c_str(), FILE_WRITE_DATA | DELETE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        stagedWriterBlocked = !stagedWriter;
        if (stagedWriterBlocked)
        {
            stagedWriterError = ::GetLastError();
        }
        stagedDeleteBlocked = ::DeleteFileW(stagedPath.c_str()) == FALSE;
        if (stagedDeleteBlocked)
        {
            stagedDeleteError = ::GetLastError();
        }
    };
    bool finalSwapAttempted{};
    Infrastructure::Windows::Detail::AtomicNativeCallResult finalSwap{};
    native.beforePublishAction = [&](const std::wstring_view destinationName) noexcept {
        if (destinationName == target.filename().wstring())
        {
            finalSwapAttempted = true;
            finalSwap = nativeRenameSameDirectory(attackerHandle.get(), destinationName, true);
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    const auto replaced = engine.replace(writePath, bytes("owned-final"), false, liveContext());
    require(stagedWriterBlocked && (stagedWriterError == ERROR_SHARING_VIOLATION ||
                                    stagedWriterError == ERROR_ACCESS_DENIED),
            "exclusive staging did not deny a concurrent writer");
    require(stagedDeleteBlocked && (stagedDeleteError == ERROR_SHARING_VIOLATION ||
                                    stagedDeleteError == ERROR_ACCESS_DENIED),
            "exclusive staging did not deny path deletion");
    require(finalSwapAttempted && finalSwap.succeeded,
            "the final-publish hook must deterministically replace the checked "
            "leaf identity");
    require(static_cast<bool>(replaced),
            replaced ? "handle-relative publish must contain a final leaf-name swap"
                     : "contained final leaf swap failed with " + replaced.error().code + ": " +
                           replaced.error().message);
    require(native.renameRequests == 1U,
            "final leaf-swap containment must publish the staged handle exactly "
            "once");
    require(readHandleText(attackerHandle.get()) == "attacker-bytes",
            "the replaced attacker object must remain confined to its "
            "already-open handle");
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "owned-final",
            "the target name must resolve to the exact staged bytes after a "
            "final leaf swap");
    require(!std::filesystem::exists(substitute),
            "the attacker substitution must remain within the retained parent "
            "directory");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementRejectsHardLinkInjectedAtFinalPublishBoundary()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"final-stage-link-target.bin";
    const auto alias = directory.path() / L"injected-stage-alias.bin";
    const auto entropy = filledEntropy(0x38U);
    const auto stagedPath = temporaryCandidate(directory.path(), entropy);

    WindowsAtomicFileStore store;
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(static_cast<bool>(store.replace(createPath, bytes("stable-old"), false, liveContext())),
            "final-stage hard-link fixture must create its target");
    Infrastructure::Windows::Detail::UniqueHandle attackerParent{::CreateFileW(
        directory.path().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(attackerParent),
            "final-stage hard-link fixture must retain a pre-authorized parent handle");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {entropy};
    bool linkAttempted{};
    bool linkCreated{};
    DWORD linkError{ERROR_SUCCESS};
    native.beforePublishAction = [&](const std::wstring_view destinationName) noexcept {
        if (destinationName == target.filename().wstring())
        {
            linkAttempted = true;
            Infrastructure::Windows::Detail::RelativeOpenOptions options{};
            options.desiredAccess = FILE_READ_ATTRIBUTES;
            options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            options.disposition =
                Infrastructure::Windows::Detail::RelativeOpenDisposition::OpenExisting;
            options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
            options.objectType = Infrastructure::Windows::Detail::RelativeObjectType::File;
            auto staged = Infrastructure::Windows::Detail::openRelative(
                attackerParent.get(), stagedPath.filename().wstring(), options);
            if (!staged)
            {
                linkError = staged.win32Error;
                return;
            }
            const auto linked =
                nativeHardLinkSameDirectory(staged.handle.get(), alias.filename().wstring());
            linkCreated = linked.succeeded;
            linkError = static_cast<DWORD>(linked.errorCode);
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto rejected =
        engine.replace(writePath, bytes("must-not-publish"), false, liveContext());
    require(linkAttempted && linkCreated,
            linkCreated ? "the final-stage hard-link injection must execute"
                        : "native FileLinkInformation did not reproduce the final-stage race: " +
                              std::to_string(linkError));
    requireError(rejected, Domain::ErrorCodes::IntegrityFailure,
                 "a hard link injected by the final prepublish hook must fail closed");
    require(native.renameRequests == 0U,
            "a detected final-stage hard link must prevent native publication");
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "stable-old",
            "final-stage hard-link rejection modified the original target");
    require(!std::filesystem::exists(stagedPath),
            "final-stage hard-link rejection left its reserved temporary name "
            "behind");

    Infrastructure::Windows::Detail::UniqueHandle aliasReader{::CreateFileW(
        alias.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(aliasReader) &&
                readHandleText(aliasReader.get()) == "must-not-publish",
            "the injected alias fixture must retain the rejected staged object's "
            "bytes");
    aliasReader.reset();
    require(::DeleteFileW(alias.c_str()) != FALSE,
            "the deterministic final-stage hard-link fixture must remove its "
            "injected alias");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementRejectsTargetReparseSwap()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / L"owned";
    const auto redirected = directory.path() / L"redirected";
    const auto junction = parent / L"junction-fixture";
    require(std::filesystem::create_directory(parent),
            "reparse-swap fixture must create its authorized parent");
    require(std::filesystem::create_directory(redirected),
            "reparse-swap fixture must create its redirected directory");
    const auto sentinel = redirected / L"sentinel.txt";
    createEmptyFixtureFile(sentinel);
    createDirectoryJunction(junction, redirected);
    require(std::filesystem::exists(junction / sentinel.filename()),
            "reparse-swap fixture junction must traverse to its destination");

    const auto target = parent / L"state.bin";
    WindowsAtomicFileStore store;
    auto targetCreate = CapabilityIssuer::issue(parent, target, Domain::FileAccess::Create);
    require(static_cast<bool>(
                store.replace(targetCreate, bytes("authorized-original"), false, liveContext())),
            "reparse-swap fixture must create the authorized target");

    Infrastructure::Windows::Detail::UniqueHandle junctionHandle{::CreateFileW(
        junction.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(junctionHandle),
            "reparse-swap fixture must retain its junction handle");

    {
        Infrastructure::Windows::Detail::UniqueHandle positiveWriter{
            ::CreateFileW(target.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(positiveWriter),
                "reparse-swap positive control must allow a writer before "
                "replacement starts");
    }

    ScriptedAtomicNativeOperations native;
    const auto stagedPath = temporaryCandidate(parent, filledEntropy(0x34U));
    native.entropyValues = {filledEntropy(0x34U)};
    bool callbackObserved{};
    bool targetDeleted{};
    bool junctionMoved{};
    DWORD mutationError{ERROR_SUCCESS};
    bool stagedWriterBlocked{};
    DWORD stagedWriterError{ERROR_SUCCESS};
    bool stagedDeleteBlocked{};
    DWORD stagedDeleteError{ERROR_SUCCESS};
    native.beforeReplacementVerificationAction = [&]() noexcept {
        callbackObserved = true;
        targetDeleted = ::DeleteFileW(target.c_str()) != FALSE;
        if (!targetDeleted)
        {
            mutationError = ::GetLastError();
        }
        else
        {
            const auto renamed =
                nativeRenameSameDirectory(junctionHandle.get(), target.filename().wstring(), false);
            junctionMoved = renamed.succeeded;
            if (!junctionMoved)
            {
                mutationError = static_cast<DWORD>(renamed.errorCode);
            }
        }
        Infrastructure::Windows::Detail::UniqueHandle stagedWriter{
            ::CreateFileW(stagedPath.c_str(), FILE_WRITE_DATA | DELETE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!stagedWriter)
        {
            stagedWriterBlocked = true;
            stagedWriterError = ::GetLastError();
        }
        stagedDeleteBlocked = ::DeleteFileW(stagedPath.c_str()) == FALSE;
        if (stagedDeleteBlocked)
        {
            stagedDeleteError = ::GetLastError();
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    auto writePath = CapabilityIssuer::issue(parent, target, Domain::FileAccess::Write);
    const auto rejected = engine.replace(writePath, bytes("must-not-commit"), false, liveContext());
    require(callbackObserved, "reparse-swap regression callback did not execute");
    require(targetDeleted && junctionMoved && mutationError == ERROR_SUCCESS,
            "reparse-swap fixture must deterministically replace the destination "
            "name");
    require(stagedWriterBlocked && (stagedWriterError == ERROR_SHARING_VIOLATION ||
                                    stagedWriterError == ERROR_ACCESS_DENIED),
            "the exclusive staged handle did not deny a concurrent staged-file "
            "writer");
    require(stagedDeleteBlocked && (stagedDeleteError == ERROR_SHARING_VIOLATION ||
                                    stagedDeleteError == ERROR_ACCESS_DENIED),
            "the exclusive staged handle did not deny path-based deletion");
    requireError(rejected, Domain::ErrorCodes::Conflict,
                 "the immediate identity recheck must reject a precommit reparse "
                 "substitution");
    require(native.renameRequests == 0U,
            "precommit reparse substitution must not reach native publication");
    require(std::filesystem::exists(target / sentinel.filename()),
            "the attacker-controlled reparse destination must remain intact "
            "after rejection");
    require(std::filesystem::exists(sentinel),
            "reparse-swap containment modified the redirected destination");
    const auto restored =
        nativeRenameSameDirectory(junctionHandle.get(), junction.filename().wstring(), false);
    require(restored.succeeded, "reparse-swap fixture must restore its junction name for cleanup");
    requireNoAtomicTemporaryFiles(parent);
}

void atomicReplacementRejectsParentReparseBeforeStaging()
{
    TemporaryDirectory directory;
    const auto redirected = directory.path() / L"redirected";
    const auto parentJunction = directory.path() / L"owned-junction";
    require(std::filesystem::create_directory(redirected),
            "parent-reparse fixture must create its redirected directory");
    const auto sentinel = redirected / L"sentinel.txt";
    createEmptyFixtureFile(sentinel);
    createDirectoryJunction(parentJunction, redirected);
    require(std::filesystem::exists(parentJunction / sentinel.filename()),
            "parent-reparse fixture junction must traverse to its destination");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {filledEntropy(0x35U)};
    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto escapedTarget = parentJunction / L"escaped.bin";
    auto createPath =
        CapabilityIssuer::issue(directory.path(), escapedTarget, Domain::FileAccess::Create);
    const auto rejected = engine.replace(createPath, bytes("must-not-stage"), false, liveContext());

    requireError(rejected, Domain::ErrorCodes::PathOutsideAuthority,
                 "atomic replacement must reject a reparse parent before staging");
    require(native.entropyRequests == 0U,
            "parent-reparse rejection generated a temporary filename");
    require(!std::filesystem::exists(redirected / escapedTarget.filename()),
            "parent-reparse rejection created the target outside authority");
    require(std::filesystem::exists(sentinel),
            "parent-reparse rejection modified the redirected destination");
    requireNoAtomicTemporaryFiles(directory.path());
    requireNoAtomicTemporaryFiles(redirected);
}

void atomicReplacementRejectsBackupIdentitySwap()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"backup-target.bin";
    const auto backup = std::filesystem::path{target.wstring() + L".bak"};
    const auto substitute = directory.path() / L"backup-substitute.bin";

    WindowsAtomicFileStore store;
    auto targetCreate =
        CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto backupCreate =
        CapabilityIssuer::issue(directory.path(), backup, Domain::FileAccess::Create);
    auto substituteCreate =
        CapabilityIssuer::issue(directory.path(), substitute, Domain::FileAccess::Create);
    require(static_cast<bool>(
                store.replace(targetCreate, bytes("authorized-original"), false, liveContext())),
            "backup-swap fixture must create the authorized target");
    require(static_cast<bool>(
                store.replace(backupCreate, bytes("authorized-backup"), false, liveContext())),
            "backup-swap fixture must create the authorized backup");
    require(static_cast<bool>(
                store.replace(substituteCreate, bytes("substituted-backup"), false, liveContext())),
            "backup-swap fixture must create the substitute backup");

    {
        Infrastructure::Windows::Detail::UniqueHandle positiveWriter{
            ::CreateFileW(backup.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(positiveWriter),
                "backup-swap positive control must allow a writer before "
                "replacement starts");
    }
    Infrastructure::Windows::Detail::UniqueHandle substituteHandle{::CreateFileW(
        substitute.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(static_cast<bool>(substituteHandle),
            "backup-swap fixture must retain its substitute handle");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {filledEntropy(0x33U), filledEntropy(0x36U)};
    bool callbackObserved{};
    bool swapSucceeded{};
    DWORD swapError{ERROR_SUCCESS};
    bool backupWriterSucceeded{};
    DWORD writerError{ERROR_SUCCESS};
    native.beforeReplacementVerificationAction = [&]() noexcept {
        callbackObserved = true;
        const auto renamed =
            nativeRenameSameDirectory(substituteHandle.get(), backup.filename().wstring(), true);
        swapSucceeded = renamed.succeeded;
        if (!swapSucceeded)
        {
            swapError = static_cast<DWORD>(renamed.errorCode);
        }
        Infrastructure::Windows::Detail::UniqueHandle backupWriter{
            ::CreateFileW(backup.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        backupWriterSucceeded = static_cast<bool>(backupWriter);
        if (!backupWriterSucceeded)
        {
            writerError = ::GetLastError();
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    const auto rejected = engine.replace(writePath, bytes("must-not-commit"), true, liveContext());
    require(callbackObserved, "backup-swap regression callback did not execute");
    require(swapSucceeded && swapError == ERROR_SUCCESS,
            "backup-swap fixture must deterministically replace the destination "
            "name");
    require(backupWriterSucceeded, "backup-swap positive control could not "
                                   "write-open the substituted destination; "
                                   "native error " +
                                       std::to_string(writerError));
    requireError(rejected, Domain::ErrorCodes::Conflict,
                 "the immediate backup identity recheck must reject substitution");
    require(native.renameRequests == 0U, "precommit backup identity substitution "
                                         "must not reach native publication");

    auto targetRead = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    auto backupRead = CapabilityIssuer::issue(directory.path(), backup, Domain::FileAccess::Read);
    require(text(take(store.read(targetRead, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "authorized-original",
            "backup identity rejection modified the target bytes");
    require(text(take(store.read(backupRead, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "substituted-backup",
            "backup identity rejection modified the attacker-substituted bytes");
    require(!std::filesystem::exists(substitute),
            "successful backup substitution must consume the attacker fixture name");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicHardLinksCannotDiscloseOrMutateOutsideAuthority()
{
    TemporaryDirectory directory;
    const auto owned = directory.path() / L"owned";
    require(std::filesystem::create_directory(owned),
            "hard-link canary must create its authorized directory");
    const auto outside = directory.path() / L"outside-secret.bin";
    const auto inside = owned / L"state.bin";

    WindowsAtomicFileStore store;
    auto outsideCreate =
        CapabilityIssuer::issue(directory.path(), outside, Domain::FileAccess::Create);
    require(static_cast<bool>(
                store.replace(outsideCreate, bytes("outside-secret"), false, liveContext())),
            "hard-link canary must create its outside source");
    require(::CreateHardLinkW(inside.c_str(), outside.c_str(), nullptr) != FALSE,
            "hard-link canary must link an outside file into the authorized root");

    auto insideRead = CapabilityIssuer::issue(owned, inside, Domain::FileAccess::Read);
    requireError(store.read(insideRead, WindowsAtomicFileStore::MaximumContentBytes, liveContext()),
                 Domain::ErrorCodes::IntegrityFailure,
                 "atomic read must reject a multiply-linked leaf before reading bytes");

    ScriptedAtomicNativeOperations replacementNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine replacementEngine{replacementNative};
    auto insideWrite = CapabilityIssuer::issue(owned, inside, Domain::FileAccess::Write);
    requireError(
        replacementEngine.replace(insideWrite, bytes("must-not-publish"), true, liveContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "atomic replacement must reject a multiply-linked target before "
        "backup copy");
    require(replacementNative.featureRequests == 0U && replacementNative.entropyRequests == 0U &&
                replacementNative.renameRequests == 0U,
            "hard-link rejection must precede capability checks, staging, and "
            "publication");
    require(!std::filesystem::exists(std::filesystem::path{inside.wstring() + L".bak"}),
            "hard-link rejection disclosed outside bytes into a recovery backup");

    {
        Infrastructure::Windows::Detail::UniqueHandle outsideReader{::CreateFileW(
            outside.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        require(static_cast<bool>(outsideReader),
                "hard-link canary must reopen its outside source directly");
        require(readHandleText(outsideReader.get()) == "outside-secret",
                "hard-link rejection mutated the outside canary bytes");
    }

    const auto staleHardLink = temporaryCandidate(owned, filledEntropy(0x58U));
    require(::CreateHardLinkW(staleHardLink.c_str(), outside.c_str(), nullptr) != FALSE,
            "stale-temp hard-link canary must create its reserved-name link");
    ScriptedAtomicNativeOperations scavengerNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine scavengerEngine{scavengerNative};
    const auto newTarget = owned / L"new-state.bin";
    auto newCreate = CapabilityIssuer::issue(owned, newTarget, Domain::FileAccess::Create);
    requireError(scavengerEngine.replace(newCreate, bytes("must-not-stage"), false, liveContext()),
                 Domain::ErrorCodes::IntegrityFailure,
                 "stale-temp scavenging must reject a multiply-linked candidate");
    require(scavengerNative.entropyRequests == 0U && scavengerNative.renameRequests == 0U,
            "stale-temp hard-link rejection must precede new staging");
    require(std::filesystem::exists(staleHardLink) && std::filesystem::exists(outside),
            "stale-temp hard-link rejection deleted a canary link");
}

void atomicTemporaryRecoveryIsBoundedAndProtectsActiveStages()
{
    TemporaryDirectory directory;
    const auto reservedTarget = temporaryCandidate(directory.path(), filledEntropy(0x50U));
    ScriptedAtomicNativeOperations reservedNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine reservedEngine{reservedNative};
    auto reservedCreate =
        CapabilityIssuer::issue(directory.path(), reservedTarget, Domain::FileAccess::Create);
    requireError(
        reservedEngine.replace(reservedCreate, bytes("must-not-create"), false, liveContext()),
        Domain::ErrorCodes::InvalidRequest,
        "public records must not enter the reserved crash-recovery namespace");
    require(reservedNative.entropyRequests == 0U && !std::filesystem::exists(reservedTarget),
            "reserved temporary-name rejection created a file");
    createEmptyFixtureFile(reservedTarget, FILE_ATTRIBUTE_TEMPORARY);
    WindowsAtomicFileStore reservedReader;
    auto reservedRead =
        CapabilityIssuer::issue(directory.path(), reservedTarget, Domain::FileAccess::Read);
    requireError(reservedReader.read(reservedRead, WindowsAtomicFileStore::MaximumContentBytes,
                                     liveContext()),
                 Domain::ErrorCodes::InvalidRequest,
                 "a crash-stage name must not be readable as a public atomic record");
    require(::DeleteFileW(reservedTarget.c_str()) != FALSE,
            "reserved-name read fixture must remove its staged object");

    const auto nonRegular = temporaryCandidate(directory.path(), filledEntropy(0x5fU));
    require(std::filesystem::create_directory(nonRegular),
            "non-regular recovery fixture must create an exact-name directory");
    ScriptedAtomicNativeOperations nonRegularNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine nonRegularEngine{nonRegularNative};
    const auto nonRegularTarget = directory.path() / L"non-regular-rejected.bin";
    auto nonRegularCreate =
        CapabilityIssuer::issue(directory.path(), nonRegularTarget, Domain::FileAccess::Create);
    requireError(
        nonRegularEngine.replace(nonRegularCreate, bytes("must-not-stage"), false, liveContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "temporary recovery must reject an exact-name non-regular object");
    require(nonRegularNative.entropyRequests == 0U && std::filesystem::is_directory(nonRegular),
            "non-regular recovery rejection mutated the directory fixture");
    require(std::filesystem::remove(nonRegular),
            "non-regular recovery fixture must remove its directory");

    const auto staleBeforeFlush = temporaryCandidate(directory.path(), filledEntropy(0x51U));
    const auto staleAfterFlush = temporaryCandidate(directory.path(), filledEntropy(0x52U));
    createEmptyFixtureFile(staleBeforeFlush, FILE_ATTRIBUTE_TEMPORARY);
    createEmptyFixtureFile(staleAfterFlush, FILE_ATTRIBUTE_NORMAL);

    ScriptedAtomicNativeOperations recoveryNative;
    recoveryNative.entropyValues = {filledEntropy(0x53U)};
    Infrastructure::Windows::Detail::AtomicReplaceEngine recoveryEngine{recoveryNative};
    const auto recoveredTarget = directory.path() / L"recovered.bin";
    auto recoveredCreate =
        CapabilityIssuer::issue(directory.path(), recoveredTarget, Domain::FileAccess::Create);
    require(static_cast<bool>(
                recoveryEngine.replace(recoveredCreate, bytes("recovered"), false, liveContext())),
            "a restart-equivalent operation must clean closed crash residues "
            "before staging");
    require(!std::filesystem::exists(staleBeforeFlush) && !std::filesystem::exists(staleAfterFlush),
            "bounded crash recovery left closed staged residues behind");

    const auto activeEntropy = filledEntropy(0x54U);
    const auto retryEntropy = filledEntropy(0x55U);
    const auto activePath = temporaryCandidate(directory.path(), activeEntropy);
    Infrastructure::Windows::Detail::UniqueHandle activeStage{
        ::CreateFileW(activePath.c_str(), FILE_READ_ATTRIBUTES | DELETE, 0U, nullptr, CREATE_NEW,
                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(activeStage),
            "active-stage safety fixture must retain an exclusive staged handle");

    ScriptedAtomicNativeOperations activeNative;
    activeNative.entropyValues = {activeEntropy, retryEntropy};
    Infrastructure::Windows::Detail::AtomicReplaceEngine activeEngine{activeNative};
    const auto activeTarget = directory.path() / L"active-safe.bin";
    auto activeCreate =
        CapabilityIssuer::issue(directory.path(), activeTarget, Domain::FileAccess::Create);
    require(static_cast<bool>(
                activeEngine.replace(activeCreate, bytes("active-safe"), false, liveContext())),
            "temporary recovery must skip a live exclusive stage and retry its "
            "name collision");
    require(activeNative.entropyRequests == 2U && std::filesystem::exists(activePath),
            "temporary recovery deleted or ignored the active-stage collision");
    activeStage.reset();

    ScriptedAtomicNativeOperations postActiveNative;
    postActiveNative.entropyValues = {filledEntropy(0x56U)};
    Infrastructure::Windows::Detail::AtomicReplaceEngine postActiveEngine{postActiveNative};
    const auto postActiveTarget = directory.path() / L"post-active.bin";
    auto postActiveCreate =
        CapabilityIssuer::issue(directory.path(), postActiveTarget, Domain::FileAccess::Create);
    require(static_cast<bool>(
                postActiveEngine.replace(postActiveCreate, bytes("cleaned"), false, liveContext())),
            "a later operation must reclaim a formerly active crash residue");
    require(!std::filesystem::exists(activePath),
            "a formerly active staged file was not reclaimed after its handle "
            "closed");

    std::vector<Infrastructure::Windows::Detail::UniqueHandle> activeBudget;
    activeBudget.reserve(
        Infrastructure::Windows::Detail::AtomicReplaceEngine::MaximumRetainedTemporaryFiles);
    for (std::size_t index = 0U;
         index <
         Infrastructure::Windows::Detail::AtomicReplaceEngine::MaximumRetainedTemporaryFiles;
         ++index)
    {
        const auto entropy = filledEntropy(static_cast<unsigned char>(0x70U + index));
        const auto path = temporaryCandidate(directory.path(), entropy);
        Infrastructure::Windows::Detail::UniqueHandle handle{
            ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | DELETE, 0U, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(handle),
                "temporary admission-bound fixture must create every live stage");
        activeBudget.push_back(std::move(handle));
    }

    ScriptedAtomicNativeOperations exhaustedNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine exhaustedEngine{exhaustedNative};
    const auto exhaustedTarget = directory.path() / L"active-budget-exhausted.bin";
    auto exhaustedCreate =
        CapabilityIssuer::issue(directory.path(), exhaustedTarget, Domain::FileAccess::Create);
    const auto exhausted =
        exhaustedEngine.replace(exhaustedCreate, bytes("must-not-stage"), false, liveContext());
    requireError(exhausted, Domain::ErrorCodes::Conflict,
                 "the live-stage budget must reject another temporary-file admission");
    require(exhausted.error().retryable && exhaustedNative.entropyRequests == 0U &&
                exhaustedNative.renameRequests == 0U && !std::filesystem::exists(exhaustedTarget),
            "temporary admission exhaustion created another staged file");

    activeBudget.clear();
    ScriptedAtomicNativeOperations boundedCleanupNative;
    boundedCleanupNative.entropyValues = {filledEntropy(0x69U)};
    Infrastructure::Windows::Detail::AtomicReplaceEngine boundedCleanupEngine{boundedCleanupNative};
    const auto boundedCleanupTarget = directory.path() / L"bounded-cleanup.bin";
    auto boundedCleanupCreate =
        CapabilityIssuer::issue(directory.path(), boundedCleanupTarget, Domain::FileAccess::Create);
    require(static_cast<bool>(boundedCleanupEngine.replace(boundedCleanupCreate, bytes("bounded"),
                                                           false, liveContext())),
            "one bounded recovery pass must reclaim the configured maximum "
            "closed residues");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicCrashRestartReclaimsClosedStagesAndProtectsLiveStage()
{
    TemporaryDirectory directory;
    const auto preFlushPath = temporaryCandidate(directory.path(), filledEntropy(0xc1U));
    const auto postFlushPath = temporaryCandidate(directory.path(), filledEntropy(0xc2U));
    const auto livePath = temporaryCandidate(directory.path(), filledEntropy(0xc3U));
    constexpr std::size_t CrashResidueCount = 2U;
    static_assert(CrashResidueCount <= Infrastructure::Windows::Detail::AtomicReplaceEngine::
                                              MaximumStaleTemporaryDeletesPerOperation);

    std::vector<wchar_t> moduleBuffer(32U * 1024U, L'\0');
    const DWORD moduleLength =
        ::GetModuleFileNameW(nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
    require(moduleLength > 0U && moduleLength < moduleBuffer.size(),
            "atomic crash test executable path must be resolved");
    const std::wstring modulePath{moduleBuffer.data(), static_cast<std::size_t>(moduleLength)};
    std::wstring commandLine = L"\"" + modulePath + L"\"";
    const std::wstring readyName =
        L"Local\\ForgeAtomicCrashReady-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetCurrentThreadId()) + L"-" + std::to_wstring(::GetTickCount64());
    Infrastructure::Windows::Detail::UniqueHandle ready{
        ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
    require(static_cast<bool>(ready), "atomic crash parent must create its readiness event");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInformation{};
    BOOL created{};
    DWORD createError{};
    {
        ScopedAtomicCrashEnvironment childMode{AtomicCrashChildVariable, L"1"};
        ScopedAtomicCrashEnvironment childRoot{AtomicCrashRootVariable,
                                                directory.path().native()};
        ScopedAtomicCrashEnvironment childReady{AtomicCrashReadyEventVariable, readyName};
        created = ::CreateProcessW(modulePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                   &processInformation);
        createError = created != FALSE ? ERROR_SUCCESS : ::GetLastError();
    }
    require(created != FALSE, std::string{"atomic crash child must launch, native error "} +
                                  std::to_string(createError));
    AtomicCrashChildProcess child{processInformation};

    const std::array<HANDLE, 2U> readinessWaits{ready.get(), child.process()};
    const DWORD readiness = ::WaitForMultipleObjects(static_cast<DWORD>(readinessWaits.size()),
                                                     readinessWaits.data(), FALSE, 15'000U);
    require(readiness == WAIT_OBJECT_0,
            readiness == WAIT_OBJECT_0 + 1U
                ? "atomic crash child exited before retaining both stage handles"
                : "atomic crash child did not reach its bounded readiness checkpoint");

    const DWORD preFlushAttributes = ::GetFileAttributesW(preFlushPath.c_str());
    const DWORD postFlushAttributes = ::GetFileAttributesW(postFlushPath.c_str());
    require(preFlushAttributes != INVALID_FILE_ATTRIBUTES &&
                (preFlushAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0U,
            "atomic crash child must leave its pre-flush stage temporary");
    require(postFlushAttributes != INVALID_FILE_ATTRIBUTES &&
                (postFlushAttributes & FILE_ATTRIBUTE_TEMPORARY) == 0U,
            "atomic crash child must leave its post-flush stage durable but noncanonical");
    for (const auto &path : {preFlushPath, postFlushPath})
    {
        Infrastructure::Windows::Detail::UniqueHandle probe{::CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        const DWORD probeError = probe ? ERROR_SUCCESS : ::GetLastError();
        require(!probe && probeError == ERROR_SHARING_VIOLATION,
                std::string{"atomic crash child must hold every production-shaped stage with no "
                            "sharing, native error "} +
                    std::to_string(probeError));
    }

    child.terminateAndClose();
    require(std::filesystem::exists(preFlushPath) && std::filesystem::exists(postFlushPath),
            "forced process death must leave both closed crash-stage names for restart recovery");

    auto retainedParent = openAtomicCrashParent(directory.path());
    auto liveStage =
        createAtomicCrashStage(retainedParent.get(), livePath, "active-live-stage", false);
    const auto liveIdentity = fileIdentity(liveStage.get());
    retainedParent.reset();

    ScriptedAtomicNativeOperations restartedNative;
    restartedNative.entropyValues = {filledEntropy(0xc4U)};
    Infrastructure::Windows::Detail::AtomicReplaceEngine restartedEngine{restartedNative};
    const auto target = directory.path() / L"restart-published.bin";
    auto createPath =
        CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    const auto restarted = restartedEngine.replace(createPath, bytes("restart-published"), false,
                                                    liveContext());
    require(static_cast<bool>(restarted),
            restarted
                ? "a fresh atomic engine must reclaim bounded closed crash residues and publish"
                : "a fresh atomic engine failed crash recovery with " + restarted.error().code +
                      ": " + restarted.error().message);
    require(restartedNative.entropyRequests == 1U && restartedNative.renameRequests == 1U,
            "restart recovery must use exactly one fresh stage and one native publication");
    require(!std::filesystem::exists(preFlushPath) &&
                !std::filesystem::exists(postFlushPath),
            "restart recovery must delete both exact closed crash-stage names");

    FILE_STANDARD_INFO liveInformation{};
    require(::GetFileInformationByHandleEx(liveStage.get(), FileStandardInfo, &liveInformation,
                                           sizeof(liveInformation)) != FALSE &&
                liveInformation.DeletePending == FALSE && liveInformation.NumberOfLinks == 1U &&
                fileIdentity(liveStage.get()) == liveIdentity && std::filesystem::exists(livePath),
            "restart recovery must preserve the exact identity of an active live stage");
    WindowsAtomicFileStore reader;
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(reader.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                  liveContext()))) == "restart-published",
            "restart recovery must expose only the complete newly published bytes");

    liveStage.reset();
    require(::DeleteFileW(livePath.c_str()) != FALSE,
            "atomic crash fixture must remove its formerly active live stage");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicTemporaryEntropyRetriesAndExhaustsCollisions()
{
    TemporaryDirectory directory;
    const auto collisionEntropy = filledEntropy(0x11U);
    const auto retryEntropy = filledEntropy(0x22U);
    const auto collisionPath = temporaryCandidate(directory.path(), collisionEntropy);
    const auto retryPath = temporaryCandidate(directory.path(), retryEntropy);
    createEmptyFixtureFile(collisionPath);
    Infrastructure::Windows::Detail::UniqueHandle activeCollision{
        ::CreateFileW(collisionPath.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | DELETE, 0U,
                      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(activeCollision),
            "atomic collision fixture must remain active during collision retries");

    ScriptedAtomicNativeOperations retryNative;
    retryNative.entropyValues = {collisionEntropy, retryEntropy};
    Infrastructure::Windows::Detail::AtomicReplaceEngine retryEngine{retryNative};
    const auto target = directory.path() / L"retry.bin";
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    require(static_cast<bool>(
                retryEngine.replace(createPath, bytes("after-collision"), false, liveContext())),
            "atomic create must retry a CSPRNG-name collision");
    require(retryNative.entropyRequests == 2U,
            "atomic collision retry did not request fresh entropy");
    FILE_STANDARD_INFO activeInformation{};
    require(::GetFileInformationByHandleEx(activeCollision.get(), FileStandardInfo,
                                           &activeInformation,
                                           sizeof(activeInformation)) != FALSE &&
                activeInformation.DeletePending == FALSE,
            "atomic collision retry modified the pre-existing active sibling");
    require(!std::filesystem::exists(retryPath),
            "successful atomic commit left its replacement name behind");

    WindowsAtomicFileStore reader;
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(text(take(reader.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                  liveContext()))) == "after-collision",
            "atomic collision retry changed the committed bytes");

    ScriptedAtomicNativeOperations exhaustedNative;
    exhaustedNative.entropyValues = {collisionEntropy};
    Infrastructure::Windows::Detail::AtomicReplaceEngine exhaustedEngine{exhaustedNative};
    const auto exhaustedTarget = directory.path() / L"exhausted.bin";
    auto exhaustedCreate =
        CapabilityIssuer::issue(directory.path(), exhaustedTarget, Domain::FileAccess::Create);
    requireError(
        exhaustedEngine.replace(exhaustedCreate, bytes("must-not-commit"), false, liveContext()),
        Domain::ErrorCodes::Conflict,
        "atomic temporary-name collision exhaustion must fail closed");
    require(exhaustedNative.entropyRequests ==
                Infrastructure::Windows::Detail::AtomicReplaceEngine::MaximumTemporaryNameAttempts,
            "atomic collision exhaustion did not honor its exact retry bound");
    require(!std::filesystem::exists(exhaustedTarget),
            "atomic collision exhaustion created the target");
}

void atomicReplacePublishesBackupBeforeTargetFailure()
{
    TemporaryDirectory directory;
    WindowsAtomicFileStore store;
    const auto target = directory.path() / L"publish-failure.bin";
    const auto backup = std::filesystem::path{target.wstring() + L".bak"};
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(static_cast<bool>(store.replace(createPath, bytes("old"), false, liveContext())),
            "publish-failure fixture must create its target");

    ScriptedAtomicNativeOperations native;
    native.entropyValues = {filledEntropy(0x41U), filledEntropy(0x42U)};
    native.failDestinationName = target.filename().wstring();
    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto failed = engine.replace(writePath, bytes("new"), true, liveContext());
    requireError(failed, Domain::ErrorCodes::Unauthorized,
                 "injected target publication failure must be reported");
    require(native.renameRequests == 2U && native.renameDestinations.size() == 2U,
            "backup and target publication must each reach one native rename");
    require(native.renameDestinations.front() == backup.filename().wstring() &&
                native.renameDestinations.back() == target.filename().wstring(),
            "the recovery backup must publish before the target rename is attempted");
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "old",
            "target publication failure modified the old target bytes");
    auto backupRead = CapabilityIssuer::issue(directory.path(), backup, Domain::FileAccess::Read);
    require(text(take(store.read(backupRead, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "old",
            "published recovery backup must contain the byte-for-byte old target");
    requireNoTemporaryAttribute(backup, "a failure-boundary recovery backup retained TEMPORARY");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementHonorsCancellationAtTargetPublishBoundary()
{
    TemporaryDirectory directory;
    WindowsAtomicFileStore store;
    const auto target = directory.path() / L"publish-cancellation.bin";
    const auto backup = std::filesystem::path{target.wstring() + L".bak"};
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(static_cast<bool>(store.replace(createPath, bytes("old"), false, liveContext())),
            "publish-cancellation fixture must create its target");

    std::stop_source cancellation;
    ScriptedAtomicNativeOperations native;
    native.entropyValues = {filledEntropy(0x43U), filledEntropy(0x44U)};
    bool targetBoundaryObserved{};
    native.beforePublishAction = [&](const std::wstring_view destinationName) noexcept {
        if (destinationName == target.filename().wstring())
        {
            targetBoundaryObserved = true;
            cancellation.request_stop();
        }
    };

    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto cancelled = engine.replace(writePath, bytes("must-not-publish"), true,
                                          liveContext(cancellation.get_token()));
    require(targetBoundaryObserved, "target prepublish cancellation hook must "
                                    "reach the final linearization boundary");
    requireError(cancelled, Domain::ErrorCodes::Cancelled,
                 "cancellation at target prepublish must abort before native publication");
    require(native.renameRequests == 1U && native.renameDestinations.size() == 1U &&
                native.renameDestinations.front() == backup.filename().wstring(),
            "prepublish cancellation must allow only the earlier recovery-backup "
            "publication");
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "old",
            "target prepublish cancellation must preserve the old target bytes");
    auto backupRead = CapabilityIssuer::issue(directory.path(), backup, Domain::FileAccess::Read);
    require(text(take(store.read(backupRead, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "old",
            "the recovery backup published before cancellation must remain valid");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementFailsClosedWithoutPosixRename()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"unsupported-volume.bin";
    WindowsAtomicFileStore store;
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(static_cast<bool>(store.replace(createPath, bytes("old"), false, liveContext())),
            "unsupported-volume fixture must create its target");

    ScriptedAtomicNativeOperations native;
    native.posixRenameSupported = false;
    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto rejected = engine.replace(writePath, bytes("must-not-stage"), true, liveContext());
    requireError(rejected, Domain::ErrorCodes::HostCapabilityUnavailable,
                 "unsupported POSIX rename must fail closed");
    require(native.featureRequests == 1U && native.entropyRequests == 0U &&
                native.renameRequests == 0U,
            "unsupported-volume rejection must precede all staging and publication");
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "old",
            "unsupported-volume rejection modified the target bytes");
    require(!std::filesystem::exists(std::filesystem::path{target.wstring() + L".bak"}),
            "unsupported-volume rejection published a backup");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementPreservesDaclCreationTimeAndRejectsAds()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / L"metadata.bin";
    WindowsAtomicFileStore store;
    auto createPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Create);
    auto writePath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Write);
    auto readPath = CapabilityIssuer::issue(directory.path(), target, Domain::FileAccess::Read);
    require(static_cast<bool>(store.replace(createPath, bytes("old"), false, liveContext())),
            "metadata fixture must create its target");

    TestDaclSnapshot expectedDacl;
    constexpr std::int64_t ExpectedCreationTime = 132'537'600'000'000'000LL;
    {
        Infrastructure::Windows::Detail::UniqueHandle metadataHandle{::CreateFileW(
            target.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(metadataHandle), "metadata fixture must open its target");

        PSECURITY_DESCRIPTOR rawDescriptor{};
        require(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &rawDescriptor, nullptr) != FALSE,
                "metadata fixture must create a deterministic DACL");
        Infrastructure::Windows::Detail::UniqueLocalAllocation<void> descriptor{rawDescriptor};
        BOOL present{};
        BOOL defaulted{};
        PACL dacl{};
        require(::GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted) !=
                        FALSE &&
                    present != FALSE && dacl != nullptr,
                "metadata fixture must extract its deterministic DACL");
        require(::SetSecurityInfo(metadataHandle.get(), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS,
                "metadata fixture must apply its deterministic DACL");
        setCreationTime(metadataHandle.get(), ExpectedCreationTime);
        expectedDacl = daclSnapshot(metadataHandle.get());
        require(expectedDacl.protectedDacl,
                "metadata fixture positive control must produce a protected DACL");
        require(creationTime(metadataHandle.get()) == ExpectedCreationTime,
                "metadata fixture positive control must set the creation time");
    }
    require(::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_ARCHIVE) != FALSE,
            "metadata fixture must set the supported archive attribute");

    const auto replaced = store.replace(writePath, bytes("new"), false, liveContext());
    require(static_cast<bool>(replaced), replaced ? "metadata-preserving replacement must succeed"
                                                  : "metadata-preserving replacement failed with " +
                                                        replaced.error().code + ": " +
                                                        replaced.error().message);
    {
        Infrastructure::Windows::Detail::UniqueHandle current{
            ::CreateFileW(target.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(current), "metadata test must reopen the published target");
        require(daclSnapshot(current.get()) == expectedDacl,
                "handle-relative replacement must preserve the exact DACL and "
                "protection bit");
        require(creationTime(current.get()) == ExpectedCreationTime,
                "handle-relative replacement must preserve the target creation time");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        require(::GetFileInformationByHandleEx(current.get(), FileAttributeTagInfo, &attributes,
                                               sizeof(attributes)) != FALSE &&
                    attributes.FileAttributes == FILE_ATTRIBUTE_ARCHIVE,
                "handle-relative replacement must preserve the supported archive "
                "attribute");
    }

    const std::filesystem::path streamPath{target.wstring() + L":private"};
    Infrastructure::Windows::Detail::UniqueHandle stream{
        ::CreateFileW(streamPath.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(static_cast<bool>(stream),
            "alternate-stream rejection fixture requires NTFS stream support");
    DWORD written{};
    constexpr char StreamBytes[] = "secret";
    require(::WriteFile(stream.get(), StreamBytes, sizeof(StreamBytes) - 1U, &written, nullptr) !=
                    FALSE &&
                written == sizeof(StreamBytes) - 1U,
            "alternate-stream fixture must write its payload");
    stream.reset();

    ScriptedAtomicNativeOperations native;
    Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
    const auto rejected = engine.replace(writePath, bytes("must-not-stage"), false, liveContext());
    requireError(rejected, Domain::ErrorCodes::IntegrityFailure,
                 "atomic replacement must reject an existing alternate data stream");
    require(native.featureRequests == 0U && native.entropyRequests == 0U &&
                native.renameRequests == 0U,
            "alternate-stream rejection must precede capability checks and staging");
    require(text(take(store.read(readPath, WindowsAtomicFileStore::MaximumContentBytes,
                                 liveContext()))) == "new",
            "alternate-stream rejection modified the target default stream");
    require(::DeleteFileW(streamPath.c_str()) != FALSE,
            "metadata fixture must remove its alternate stream before "
            "compression testing");

    {
        Infrastructure::Windows::Detail::UniqueHandle compressionHandle{
            ::CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(compressionHandle),
                "compression rejection fixture must open its target");
        USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD returned{};
        require(::DeviceIoControl(compressionHandle.get(), FSCTL_SET_COMPRESSION, &format,
                                  sizeof(format), nullptr, 0U, &returned, nullptr) != FALSE,
                "compression rejection fixture requires NTFS compression support");
    }
    require((::GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_COMPRESSED) != 0U,
            "compression rejection positive control must set the compressed "
            "attribute");

    ScriptedAtomicNativeOperations compressedNative;
    Infrastructure::Windows::Detail::AtomicReplaceEngine compressedEngine{compressedNative};
    const auto compressedRejected =
        compressedEngine.replace(writePath, bytes("must-not-stage"), false, liveContext());
    requireError(compressedRejected, Domain::ErrorCodes::IntegrityFailure,
                 "atomic replacement must reject an existing compressed file");
    require(compressedNative.featureRequests == 0U && compressedNative.entropyRequests == 0U &&
                compressedNative.renameRequests == 0U,
            "compressed-file rejection must precede capability checks and staging");
    requireNoAtomicTemporaryFiles(directory.path());
}

void atomicReplacementRejectsExtendedAttributesAndSecurityIdentityMismatch()
{
    require(static_cast<bool>(
                Infrastructure::Windows::Detail::AtomicReplaceEngine::validateExtendedAttributeSize(
                    0U)),
            "zero-length EA policy positive control must succeed");
    requireError(
        Infrastructure::Windows::Detail::AtomicReplaceEngine::validateExtendedAttributeSize(1U),
        Domain::ErrorCodes::IntegrityFailure,
        "nonzero EA metadata must be rejected by the production validation seam");
    require(static_cast<bool>(Infrastructure::Windows::Detail::AtomicReplaceEngine::
                                  validateSecurityIdentityEquivalence(true, true, true)),
            "equivalent security identity policy positive control must succeed");
    requireError(
        Infrastructure::Windows::Detail::AtomicReplaceEngine::validateSecurityIdentityEquivalence(
            false, true, true),
        Domain::ErrorCodes::IntegrityFailure,
        "owner mismatch must be rejected by the production validation seam");
    requireError(
        Infrastructure::Windows::Detail::AtomicReplaceEngine::validateSecurityIdentityEquivalence(
            true, true, false),
        Domain::ErrorCodes::IntegrityFailure,
        "mandatory-label mismatch must be rejected by the production "
        "validation seam");

    TemporaryDirectory directory;
    WindowsAtomicFileStore store;

    const auto eaTarget = directory.path() / L"extended-attributes.bin";
    auto eaCreate = CapabilityIssuer::issue(directory.path(), eaTarget, Domain::FileAccess::Create);
    auto eaWrite = CapabilityIssuer::issue(directory.path(), eaTarget, Domain::FileAccess::Write);
    require(static_cast<bool>(store.replace(eaCreate, bytes("ea-old"), false, liveContext())),
            "EA policy fixture must create its target");
    DWORD eaError{ERROR_SUCCESS};
    bool eaSupported{};
    {
        Infrastructure::Windows::Detail::UniqueHandle eaHandle{
            ::CreateFileW(eaTarget.c_str(), FILE_READ_EA | FILE_WRITE_EA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(eaHandle), "EA policy fixture must open its target");
        eaSupported = setFixtureExtendedAttribute(eaHandle.get(), eaError);
    }
    std::cout << "[EVIDENCE] atomic_extended_attributes_supported=" << (eaSupported ? 1 : 0)
              << " native_error=" << eaError << '\n';
    if (eaSupported)
    {
        ScriptedAtomicNativeOperations native;
        Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
        requireError(engine.replace(eaWrite, bytes("must-not-stage"), false, liveContext()),
                     Domain::ErrorCodes::IntegrityFailure,
                     "an existing target with EAs must be rejected before staging");
        require(native.featureRequests == 0U && native.entropyRequests == 0U &&
                    native.renameRequests == 0U,
                "EA rejection must precede capability checks, staging, and "
                "publication");
        Infrastructure::Windows::Detail::UniqueHandle directReader{::CreateFileW(
            eaTarget.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        require(static_cast<bool>(directReader) && readHandleText(directReader.get()) == "ea-old",
                "EA rejection mutated the target default stream");
    }

    const auto labelTarget = directory.path() / L"mandatory-label.bin";
    auto labelCreate =
        CapabilityIssuer::issue(directory.path(), labelTarget, Domain::FileAccess::Create);
    auto labelWrite =
        CapabilityIssuer::issue(directory.path(), labelTarget, Domain::FileAccess::Write);
    require(static_cast<bool>(store.replace(labelCreate, bytes("label-old"), false, liveContext())),
            "mandatory-label fixture must create its target");
    DWORD labelError{ERROR_SUCCESS};
    bool labelSupported{};
    {
        Infrastructure::Windows::Detail::UniqueHandle labelHandle{
            ::CreateFileW(labelTarget.c_str(), READ_CONTROL | WRITE_OWNER,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(labelHandle), "mandatory-label fixture must open its target");
        labelSupported = setFixtureLowMandatoryLabel(labelHandle.get(), labelError);
    }
    std::cout << "[EVIDENCE] atomic_mandatory_label_fixture_supported=" << (labelSupported ? 1 : 0)
              << " native_error=" << labelError << '\n';
    if (labelSupported)
    {
        ScriptedAtomicNativeOperations native;
        Infrastructure::Windows::Detail::AtomicReplaceEngine engine{native};
        requireError(engine.replace(labelWrite, bytes("must-not-stage"), false, liveContext()),
                     Domain::ErrorCodes::IntegrityFailure,
                     "a staged security-label mismatch must fail before publication");
        require(native.renameRequests == 0U, "mandatory-label mismatch reached native publication");
        Infrastructure::Windows::Detail::UniqueHandle directReader{::CreateFileW(
            labelTarget.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        require(static_cast<bool>(directReader) &&
                    readHandleText(directReader.get()) == "label-old",
                "mandatory-label mismatch mutated the target bytes");
    }
}

void configurationPreservesUnknownFieldsAndUsesBackup()
{
    ConfigurationFixture fixture;
    MemoryAtomicFileStore files;
    files.exists = true;
    files.content = bytes(
        R"({"schema_version":1,"future":{"mode":"kept"},"dashboard":{"future_mode":7,"port":7788}})");
    WindowsConfigurationStore store{files, fixture.readPath, fixture.writePath, fixture.createPath,
                                    fixture.backupReadPath};
    const auto loaded = take(store.load(liveContext()));
    require(loaded.dashboard.port == 7788, "configuration load must decode known fields");

    Domain::AppConfigPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(8123);
    const auto updated = take(store.update(patch, liveContext()));
    require(updated.dashboard.port == 8123, "configuration update must publish its typed value");
    require(files.lastAccess == Domain::FileAccess::Write,
            "existing configuration update must use write authority");
    require(files.lastRetainBackup, "configuration update must request an atomic backup");
    const auto document = nlohmann::json::parse(text(files.content));
    require(document.at("future").at("mode") == "kept",
            "configuration update must preserve unknown top-level fields");
    require(document.at("dashboard").at("future_mode") == 7,
            "configuration update must preserve unknown nested fields");
    require(document.at("dashboard").at("port") == 8123,
            "configuration update must replace the patched known field");
}

void configurationRecoversOnlyFromValidBackup()
{
    ConfigurationFixture fixture;
    MemoryAtomicFileStore files;
    files.exists = true;
    files.content = bytes("{corrupt-primary}");
    files.backupExists = true;
    files.backupContent =
        bytes(R"({"schema_version":1,"recovered":{"future":true},"dashboard":{"port":8451}})");
    WindowsConfigurationStore recovered{files, fixture.readPath, fixture.writePath,
                                        fixture.createPath, fixture.backupReadPath};
    require(take(recovered.load(liveContext())).dashboard.port == 8451,
            "a corrupt primary must recover from its valid sibling backup");

    Domain::AppConfigPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(8452);
    require(take(recovered.update(patch, liveContext())).dashboard.port == 8452,
            "a backup-recovered snapshot must remain updateable");
    const auto document = nlohmann::json::parse(text(files.content));
    require(document.at("recovered").at("future") == true,
            "backup recovery must preserve unknown fields in the live document");

    MemoryAtomicFileStore corruptPairFiles;
    corruptPairFiles.exists = true;
    corruptPairFiles.content = bytes("{corrupt-primary}");
    corruptPairFiles.backupExists = true;
    corruptPairFiles.backupContent = bytes("{corrupt-backup}");
    WindowsConfigurationStore corruptPair{corruptPairFiles, fixture.readPath, fixture.writePath,
                                          fixture.createPath, fixture.backupReadPath};
    requireError(corruptPair.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "a corrupt primary and corrupt backup must fail with integrity_failure");
}

void configurationRejectsCorruptionDuplicatesDepthAndSecrets()
{
    ConfigurationFixture fixture;
    MemoryAtomicFileStore files;
    files.exists = true;
    files.content = bytes("{not-json}");
    WindowsConfigurationStore corrupt{files, fixture.readPath, fixture.writePath,
                                      fixture.createPath, fixture.backupReadPath};
    requireError(corrupt.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "corrupt configuration without a backup must fail with "
                 "integrity_failure");

    files.content = bytes(R"({"schema_version":1,"future":1,"future":2})");
    WindowsConfigurationStore duplicate{files, fixture.readPath, fixture.writePath,
                                        fixture.createPath, fixture.backupReadPath};
    requireError(duplicate.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "duplicate configuration keys without a backup must fail closed");

    std::string deep = R"({"schema_version":1,"future":)";
    deep.append(34U, '[');
    deep.push_back('0');
    deep.append(34U, ']');
    deep.push_back('}');
    files.content = bytes(deep);
    WindowsConfigurationStore excessiveDepth{files, fixture.readPath, fixture.writePath,
                                             fixture.createPath, fixture.backupReadPath};
    requireError(excessiveDepth.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "configuration depth cap+1 without a backup must fail closed");

    files.content = bytes(R"({"schema_version":1,"future_token":"must-not-persist"})");
    WindowsConfigurationStore secretField{files, fixture.readPath, fixture.writePath,
                                          fixture.createPath, fixture.backupReadPath};
    requireError(secretField.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "secret-bearing configuration without a backup must fail closed");
}

void configurationCommitBeforePublishAndLateCancellation()
{
    ConfigurationFixture fixture;
    MemoryAtomicFileStore files;
    files.exists = true;
    files.content = bytes(R"({"schema_version":1,"dashboard":{"port":7788}})");
    WindowsConfigurationStore store{files, fixture.readPath, fixture.writePath, fixture.createPath,
                                    fixture.backupReadPath};
    require(take(store.load(liveContext())).dashboard.port == 7788,
            "initial configuration must load");

    Domain::AppConfigPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(9000);
    files.failReplace = true;
    requireError(store.update(patch, liveContext()), Domain::ErrorCodes::StorageFull,
                 "failed durable replacement must fail the update");
    require(take(store.load(liveContext())).dashboard.port == 7788,
            "failed durable replacement must not publish the candidate snapshot");

    std::stop_source lateCancellation;
    files.failReplace = false;
    files.cancelAfterCommit = &lateCancellation;
    const auto committed = store.update(patch, liveContext(lateCancellation.get_token()));
    require(committed && committed.value().dashboard.port == 9000,
            "cancellation after durable linearization must still return success");
    require(lateCancellation.stop_requested(), "late-cancellation test seam must have fired");
    require(take(store.load(liveContext())).dashboard.port == 9000,
            "committed configuration must publish its matching snapshot");
}

void configurationBoundsMissingDefaultsAndShutdown()
{
    ConfigurationFixture fixture;
    MemoryAtomicFileStore missingFiles;
    WindowsConfigurationStore missing{missingFiles, fixture.readPath, fixture.writePath,
                                      fixture.createPath, fixture.backupReadPath};
    require(take(missing.load(liveContext())) == Domain::defaultAppConfig(),
            "missing configuration must produce explicit typed defaults");

    Domain::AppConfigPatch firstPatch;
    firstPatch.dashboardPort = static_cast<std::uint16_t>(8101);
    require(take(missing.update(firstPatch, liveContext())).dashboard.port == 8101,
            "first missing-file update must commit");
    require(missingFiles.lastAccess == Domain::FileAccess::Create,
            "first missing-file update must use create authority");
    Domain::AppConfigPatch secondPatch;
    secondPatch.dashboardPort = static_cast<std::uint16_t>(8102);
    require(take(missing.update(secondPatch, liveContext())).dashboard.port == 8102,
            "second configuration update must commit");
    require(missingFiles.lastAccess == Domain::FileAccess::Write,
            "second configuration update must use write authority");

    MemoryAtomicFileStore oversizedFiles;
    oversizedFiles.exists = true;
    oversizedFiles.content.resize(WindowsConfigurationStore::MaximumDocumentBytes + 1U);
    WindowsConfigurationStore oversized{oversizedFiles, fixture.readPath, fixture.writePath,
                                        fixture.createPath, fixture.backupReadPath};
    requireError(oversized.load(liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "configuration document cap+1 without a backup must fail closed");

    const auto mismatchedPath =
        CapabilityIssuer::issue(fixture.directory.path(), fixture.directory.path() / L"other.json",
                                Domain::FileAccess::Write);
    bool rejectedMismatchedBinding = false;
    try
    {
        WindowsConfigurationStore mismatched{missingFiles, fixture.readPath, mismatchedPath,
                                             fixture.createPath, fixture.backupReadPath};
        static_cast<void>(mismatched);
    }
    catch (const std::invalid_argument &)
    {
        rejectedMismatchedBinding = true;
    }
    require(rejectedMismatchedBinding,
            "configuration accepted capabilities bound to different files");

    const auto mismatchedBackup = CapabilityIssuer::issue(
        fixture.directory.path(), fixture.directory.path() / L"other.json.bak",
        Domain::FileAccess::Read);
    bool rejectedMismatchedBackup = false;
    try
    {
        WindowsConfigurationStore mismatched{missingFiles, fixture.readPath, fixture.writePath,
                                             fixture.createPath, mismatchedBackup};
        static_cast<void>(mismatched);
    }
    catch (const std::invalid_argument &)
    {
        rejectedMismatchedBackup = true;
    }
    require(rejectedMismatchedBackup,
            "configuration accepted a backup capability for a different sibling");

    missing.shutdown();
    requireError(missing.load(liveContext()), Domain::ErrorCodes::TransportClosed,
                 "configuration calls after shutdown must fail closed");
}

void dpapiRejectsDifferentEffectiveSid()
{
    RegistryScope registry;
    constexpr std::string_view LogicalKey{"different-sid"};
    const auto secret = bytes("different-sid-secret-canary");
    DpapiSecureStorage storage{registry.subkey()};
    require(static_cast<bool>(storage.put(LogicalKey, secret, liveContext())),
            "DPAPI different-SID fixture must protect its secret");
    const auto ownerRoundTrip = take(storage.get(LogicalKey, secret.size(), liveContext()));
    require(ownerRoundTrip && *ownerRoundTrip == secret,
            "DPAPI different-SID fixture must decrypt for its owner");

    BCryptSha256Hasher hasher;
    const auto logicalKeyBytes = bytes(LogicalKey);
    const auto digestText = take(hasher.sha256(logicalKeyBytes)).value();
    require(digestText.size() == 64U, "SHA-256 digest did not contain 64 hex characters");
    std::array<std::byte, 32> entropy{};
    for (std::size_t index = 0U; index < entropy.size(); ++index)
    {
        entropy[index] = hexByte(digestText[index * 2U], digestText[(index * 2U) + 1U]);
    }
    std::wstring valueName{L"v1_"};
    valueName.reserve(3U + digestText.size());
    for (const char character : digestText)
    {
        valueName.push_back(static_cast<wchar_t>(character));
    }

    auto openedRegistry = take(Infrastructure::Windows::Detail::UniqueRegistryKey::openCurrentUser(
        registry.subkey(), KEY_QUERY_VALUE));
    require(openedRegistry.has_value(), "DPAPI different-SID fixture registry key was missing");
    auto registryKey = std::move(openedRegistry.value());
    DWORD storedType{};
    DWORD storedByteCount{};
    auto status = RegQueryValueExW(registryKey.get(), valueName.c_str(), nullptr, &storedType,
                                   nullptr, &storedByteCount);
    require(status == ERROR_SUCCESS && storedType == REG_BINARY,
            "DPAPI different-SID fixture value was unavailable");
    constexpr DWORD MaximumStoredFixtureBytes =
        static_cast<DWORD>((2U * DpapiSecureStorage::MaximumSecretBytes) + 5U);
    require(storedByteCount > 5U && storedByteCount <= MaximumStoredFixtureBytes,
            "DPAPI different-SID fixture value exceeded its bound");
    std::vector<BYTE> stored(storedByteCount);
    status = RegQueryValueExW(registryKey.get(), valueName.c_str(), nullptr, &storedType,
                              stored.data(), &storedByteCount);
    require(status == ERROR_SUCCESS && storedType == REG_BINARY,
            "DPAPI different-SID fixture value could not be read");
    stored.resize(storedByteCount);

    HANDLE rawOwnerToken{};
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawOwnerToken) != FALSE,
            "could not open the DPAPI owner process token");
    Infrastructure::Windows::Detail::UniqueHandle ownerToken{rawOwnerToken};
    auto ownerUserInformation = tokenUserInformation(ownerToken.get());
    const auto *ownerUser = reinterpret_cast<const TOKEN_USER *>(ownerUserInformation.get());
    const auto ownerSid = sidText(ownerUser->User.Sid);

    DWORD unprotectError = ERROR_SUCCESS;
    std::string alternateSid;
    {
        ScopedAnonymousImpersonation impersonation;
        HANDLE rawAlternateToken{};
        require(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &rawAlternateToken) != FALSE,
                "could not open the alternate effective token");
        Infrastructure::Windows::Detail::UniqueHandle alternateToken{rawAlternateToken};
        auto alternateUserInformation = tokenUserInformation(alternateToken.get());
        const auto *alternateUser =
            reinterpret_cast<const TOKEN_USER *>(alternateUserInformation.get());
        alternateSid = sidText(alternateUser->User.Sid);
        require(alternateSid == "S-1-5-7",
                "anonymous impersonation did not yield the fixed alternate SID");
        require(EqualSid(ownerUser->User.Sid, alternateUser->User.Sid) == FALSE,
                "DPAPI denial test did not obtain a genuinely different SID");

        DATA_BLOB encrypted{static_cast<DWORD>(stored.size() - 5U), stored.data() + 5U};
        DATA_BLOB entropyBlob{static_cast<DWORD>(entropy.size()),
                              reinterpret_cast<BYTE *>(entropy.data())};
        DATA_BLOB plaintext{};
        SetLastError(ERROR_SUCCESS);
        const BOOL unprotected = CryptUnprotectData(&encrypted, nullptr, &entropyBlob, nullptr,
                                                    nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plaintext);
        unprotectError = unprotected ? ERROR_SUCCESS : GetLastError();
        Infrastructure::Windows::Detail::UniqueLocalAllocation<BYTE> plaintextOwner{
            plaintext.pbData};
        if (plaintext.pbData != nullptr && plaintext.cbData != 0U)
        {
            SecureZeroMemory(plaintext.pbData, plaintext.cbData);
        }
        require(!unprotected, "a genuinely different Windows SID decrypted owner ciphertext");
        require(unprotectError == ERROR_ACCESS_DENIED,
                "different-SID DPAPI denial did not return access denied");
        require(impersonation.revert(), "could not revert alternate-SID impersonation");
    }

    std::cout << "[EVIDENCE] dpapi_owner_sid=" << ownerSid << " alternate_sid=" << alternateSid
              << " unprotect_error=" << unprotectError << '\n';
}

void dpapiRoundTripBoundsRemoveAndShutdown()
{
    RegistryScope registry;
    DpapiSecureStorage storage{registry.subkey()};
    const std::vector<std::byte> secret{std::byte{0x00}, std::byte{0x11}, std::byte{0xFE},
                                        std::byte{0xFF}};
    require(static_cast<bool>(storage.put("roundtrip", secret, liveContext())),
            "DPAPI put must succeed for current user");
    const auto restored = take(storage.get("roundtrip", secret.size(), liveContext()));
    require(restored && *restored == secret, "DPAPI get must return the exact current-user secret");
    require(!take(storage.get("missing", 1U, liveContext())),
            "DPAPI missing key must return nullopt");
    require(static_cast<bool>(storage.remove("roundtrip", liveContext())),
            "DPAPI remove must succeed");
    require(!take(storage.get("roundtrip", secret.size(), liveContext())),
            "DPAPI removed key must be absent");

    std::string maximumKey(DpapiSecureStorage::MaximumKeyBytes, 'k');
    require(static_cast<bool>(storage.put(maximumKey, {}, liveContext())),
            "DPAPI exact key cap must succeed");
    maximumKey.push_back('k');
    requireError(storage.put(maximumKey, {}, liveContext()), Domain::ErrorCodes::PayloadTooLarge,
                 "DPAPI key cap+1 must fail with payload_too_large");

    std::vector<std::byte> maximumSecret(DpapiSecureStorage::MaximumSecretBytes, std::byte{0x5A});
    require(static_cast<bool>(storage.put("maximum-secret", maximumSecret, liveContext())),
            "DPAPI exact secret cap must succeed");
    maximumSecret.push_back(std::byte{0x5A});
    requireError(storage.put("excessive-secret", maximumSecret, liveContext()),
                 Domain::ErrorCodes::PayloadTooLarge,
                 "DPAPI secret cap+1 must fail with payload_too_large");
    requireError(
        storage.get("maximum-secret", DpapiSecureStorage::MaximumSecretBytes + 1U, liveContext()),
        Domain::ErrorCodes::LimitExceeded,
        "DPAPI caller output cap+1 must fail with limit_exceeded");

    storage.shutdown();
    requireError(storage.get("maximum-secret", 1U, liveContext()),
                 Domain::ErrorCodes::TransportClosed,
                 "DPAPI calls after shutdown must fail closed");
}

void dpapiEntryCapAllowsOverwriteAndRejectsCorruption()
{
    RegistryScope registry;
    DpapiSecureStorage storage{registry.subkey()};
    require(static_cast<bool>(storage.put("existing", bytes("one"), liveContext())),
            "DPAPI cap fixture must create one valid entry");

    HKEY rawKey{};
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, registry.subkey().c_str(), 0,
                                      KEY_QUERY_VALUE | KEY_SET_VALUE, &rawKey);
    require(opened == ERROR_SUCCESS, "DPAPI test registry must open");

    DWORD existingNameCharacters = 68U;
    std::vector<wchar_t> existingNameBuffer(existingNameCharacters, L'\0');
    require(RegEnumValueW(rawKey, 0, existingNameBuffer.data(), &existingNameCharacters, nullptr,
                          nullptr, nullptr, nullptr) == ERROR_SUCCESS,
            "DPAPI existing hashed value must enumerate");
    const std::wstring existingName{existingNameBuffer.data(),
                                    static_cast<std::size_t>(existingNameCharacters)};
    const auto fillerBlob = fixtureStoredBlob();
    std::size_t fillerCount{};
    std::uint64_t suffix = 1U;
    while (fillerCount < DpapiSecureStorage::MaximumEntryCount - 1U)
    {
        const auto name = fixtureStoredValueName(suffix++);
        if (name == existingName)
        {
            continue;
        }
        require(RegSetValueExW(rawKey, name.c_str(), 0, REG_BINARY, fillerBlob.data(),
                               static_cast<DWORD>(fillerBlob.size())) == ERROR_SUCCESS,
                "DPAPI structurally valid registry filler must succeed");
        ++fillerCount;
    }
    RegCloseKey(rawKey);

    require(static_cast<bool>(storage.put("existing", bytes("two"), liveContext())),
            "DPAPI overwrite at the 128-entry cap must succeed");
    requireError(storage.put("new-entry", bytes("x"), liveContext()),
                 Domain::ErrorCodes::LimitExceeded,
                 "DPAPI new entry at cap must fail with limit_exceeded");

    RegistryScope corruptRegistry;
    DpapiSecureStorage corrupt{corruptRegistry.subkey()};
    require(static_cast<bool>(corrupt.put("tampered", bytes("secret"), liveContext())),
            "DPAPI corruption fixture must create a value");
    HKEY corruptKey{};
    require(RegOpenKeyExW(HKEY_CURRENT_USER, corruptRegistry.subkey().c_str(), 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &corruptKey) == ERROR_SUCCESS,
            "DPAPI corruption registry must open");
    DWORD valueNameCharacters = 256U;
    std::vector<wchar_t> valueName(valueNameCharacters, L'\0');
    require(RegEnumValueW(corruptKey, 0, valueName.data(), &valueNameCharacters, nullptr, nullptr,
                          nullptr, nullptr) == ERROR_SUCCESS,
            "DPAPI hashed registry value must enumerate");
    valueName.resize(valueNameCharacters);
    const std::wstring valueNameText{valueName.data(), valueNameCharacters};
    const BYTE damaged = 0U;
    require(RegSetValueExW(corruptKey, valueNameText.c_str(), 0, REG_BINARY, &damaged, 1U) ==
                ERROR_SUCCESS,
            "DPAPI corruption fixture must overwrite the blob");
    RegCloseKey(corruptKey);
    requireError(corrupt.get("tampered", 64U, liveContext()), Domain::ErrorCodes::IntegrityFailure,
                 "DPAPI corrupt binary envelope must fail with integrity_failure");
}
void dpapiRejectsMalformedRegistryCatalogEntries()
{
    {
        RegistryScope invalidNameRegistry;
        HKEY registryKey{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER, invalidNameRegistry.subkey().c_str(), 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &registryKey,
                                nullptr) == ERROR_SUCCESS,
                "DPAPI malformed-name registry fixture must open");
        const auto storedBlob = fixtureStoredBlob();
        require(RegSetValueExW(registryKey, L"invalid-name", 0, REG_BINARY, storedBlob.data(),
                               static_cast<DWORD>(storedBlob.size())) == ERROR_SUCCESS,
                "DPAPI malformed-name registry fixture must write");
        RegCloseKey(registryKey);

        DpapiSecureStorage storage{invalidNameRegistry.subkey()};
        requireError(storage.put("candidate", bytes("x"), liveContext()),
                     Domain::ErrorCodes::IntegrityFailure,
                     "DPAPI put must reject a malformed registry value name");
    }

    {
        RegistryScope invalidTypeRegistry;
        HKEY registryKey{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER, invalidTypeRegistry.subkey().c_str(), 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &registryKey,
                                nullptr) == ERROR_SUCCESS,
                "DPAPI malformed-type registry fixture must open");
        const auto validName = fixtureStoredValueName(9'001U);
        constexpr wchar_t InvalidText[] = L"not binary";
        require(RegSetValueExW(registryKey, validName.c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(InvalidText),
                               static_cast<DWORD>(sizeof(InvalidText))) == ERROR_SUCCESS,
                "DPAPI malformed-type registry fixture must write");
        RegCloseKey(registryKey);

        DpapiSecureStorage storage{invalidTypeRegistry.subkey()};
        requireError(storage.put("candidate", bytes("x"), liveContext()),
                     Domain::ErrorCodes::IntegrityFailure,
                     "DPAPI put must reject a non-binary registry value");
    }
}

} // namespace

void registerStorageWindowsTests(TestRegistry &tests)
{
    if (atomicCrashChildRequested())
    {
        tests.clear();
        addTest(tests, "storage.atomic.crash-recovery-child", atomicCrashRecoveryChild);
        return;
    }
    addTest(tests, "storage.atomic.roundtrip-backup-bounds", atomicReplaceRoundTripBackupAndBounds);
    addTest(tests, "storage.atomic.security-cancellation",
            atomicReplaceRejectsMissingWriteAdsAndCancellation);
    addTest(tests, "storage.atomic.parent-anchor", atomicReplacePinsParentAncestry);
    addTest(tests, "storage.atomic.read-parent-anchor",
            atomicReadPinsParentAncestryAndUsesRelativeLeafOpen);
    addTest(tests, "storage.atomic.case-sensitive-directory-policy",
            atomicRejectsCaseSensitiveDirectoryAuthority);
    addTest(tests, "storage.atomic.parent-reparse-no-stage",
            atomicReplacementRejectsParentReparseBeforeStaging);
    addTest(tests, "storage.atomic.old-handle-new-identity",
            atomicReplacementKeepsOldHandleAndPublishesNewIdentity);
    addTest(tests, "storage.atomic.reader-without-delete-share",
            atomicReplacementReturnsRetryableConflictForNonDeleteSharingReader);
    addTest(tests, "storage.atomic.final-leaf-swap-containment",
            atomicReplacementContainsFinalLeafSwapAndTempTamper);
    addTest(tests, "storage.atomic.final-stage-hard-link-rejection",
            atomicReplacementRejectsHardLinkInjectedAtFinalPublishBoundary);
    addTest(tests, "storage.atomic.target-reparse-swap", atomicReplacementRejectsTargetReparseSwap);
    addTest(tests, "storage.atomic.backup-identity-swap",
            atomicReplacementRejectsBackupIdentitySwap);
    addTest(tests, "storage.atomic.unsupported-posix-no-stage",
            atomicReplacementFailsClosedWithoutPosixRename);
    addTest(tests, "storage.atomic.metadata-policy",
            atomicReplacementPreservesDaclCreationTimeAndRejectsAds);
    addTest(tests, "storage.atomic.security-identity-ea-policy",
            atomicReplacementRejectsExtendedAttributesAndSecurityIdentityMismatch);
    addTest(tests, "storage.atomic.hard-link-authority",
            atomicHardLinksCannotDiscloseOrMutateOutsideAuthority);
    addTest(tests, "storage.atomic.stale-temp-recovery",
            atomicTemporaryRecoveryIsBoundedAndProtectsActiveStages);
    addTest(tests, "storage.atomic.crash-restart-recovery",
            atomicCrashRestartReclaimsClosedStagesAndProtectsLiveStage);
    addTest(tests, "storage.atomic.entropy-collision-bound",
            atomicTemporaryEntropyRetriesAndExhaustsCollisions);
    addTest(tests, "storage.atomic.backup-before-target-failure",
            atomicReplacePublishesBackupBeforeTargetFailure);
    addTest(tests, "storage.atomic.target-prepublish-cancellation",
            atomicReplacementHonorsCancellationAtTargetPublishBoundary);
    addTest(tests, "storage.config.unknown-field-preservation",
            configurationPreservesUnknownFieldsAndUsesBackup);
    addTest(tests, "storage.config.valid-backup-recovery",
            configurationRecoversOnlyFromValidBackup);
    addTest(tests, "storage.config.hostile-json",
            configurationRejectsCorruptionDuplicatesDepthAndSecrets);
    addTest(tests, "storage.config.commit-before-publish",
            configurationCommitBeforePublishAndLateCancellation);
    addTest(tests, "storage.config.bounds-defaults-shutdown",
            configurationBoundsMissingDefaultsAndShutdown);
    addTest(tests, "storage.dpapi.roundtrip-bounds-shutdown",
            dpapiRoundTripBoundsRemoveAndShutdown);
    addTest(tests, "storage.dpapi.different-sid-denial", dpapiRejectsDifferentEffectiveSid);
    addTest(tests, "storage.dpapi.entry-cap-corruption",
            dpapiEntryCapAllowsOverwriteAndRejectsCorruption);
    addTest(tests, "storage.dpapi.catalog-integrity", dpapiRejectsMalformedRegistryCatalogEntries);
}

} // namespace ForgeConductor::Tests
