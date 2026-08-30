#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBearerToken.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

constexpr std::size_t MaximumPathCharacters = 32U * 1024U;
constexpr auto StartupTimeout = 45s;
constexpr auto ProcessExitTimeout = 30s;
constexpr auto CleanupProcessExitTimeout = 15s;
constexpr auto InfrastructureReleaseTimeout = 15s;
constexpr auto PollInterval = 50ms;
constexpr DWORD FixtureOwnershipConflictExitCode = 3U;
constexpr DWORD ForcedTerminationExitCode = 0xF0160001U;
constexpr std::uint16_t ExpectedDashboardPort = 7788U;
constexpr std::uint64_t MaximumQualifiedExecutableBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::string_view ExpectedShutdownAcknowledgement =
    "{\"ok\":true,\"message\":\"Manager shutting down\",\"state\":\"stopping\"}";
constexpr std::wstring_view RegistryParent = L"Software\\Forge Conductor";
constexpr std::wstring_view RegistryLeafPrefix =
    L"Software\\Forge Conductor\\ManagerLifecycleTest.";

[[noreturn]] void fail(
    const std::string& message,
    const std::source_location location = std::source_location::current())
{
    throw std::runtime_error{
        message + " at " + location.file_name() + ':' +
        std::to_string(location.line())};
}

void require(
    const bool condition,
    const std::string& message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        fail(message, location);
    }
}

template <typename Value>
[[nodiscard]] Value take(
    Domain::Result<Value> result,
    const std::string_view action = {})
{
    if (!result) {
        const std::string prefix = action.empty()
            ? std::string{}
            : std::string{action} + ": ";
        fail(prefix + result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

void requireSuccess(
    Domain::Result<void> result,
    const std::string_view action)
{
    if (!result) {
        fail(
            std::string{action} + ": " + result.error().code + ": " +
            result.error().message);
    }
}

class UniqueHandle final {
public:
    explicit UniqueHandle(
        const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : value_{std::exchange(other.value_, INVALID_HANDLE_VALUE)}
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid()) {
            static_cast<void>(::CloseHandle(value_));
        }
        value_ = replacement;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class UniqueFindHandle final {
public:
    explicit UniqueFindHandle(
        const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueFindHandle() noexcept
    {
        if (valid()) {
            static_cast<void>(::FindClose(value_));
        }
    }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

    void reset() noexcept
    {
        if (valid()) {
            static_cast<void>(::FindClose(value_));
            value_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class UniqueInternetHandle final {
public:
    explicit UniqueInternetHandle(const HINTERNET value = nullptr) noexcept
        : value_{value}
    {
    }

    ~UniqueInternetHandle() noexcept
    {
        if (value_ != nullptr) {
            static_cast<void>(::WinHttpCloseHandle(value_));
        }
    }

    UniqueInternetHandle(const UniqueInternetHandle&) = delete;
    UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

private:
    HINTERNET value_{};
};

class UniqueSocket final {
public:
    explicit UniqueSocket(const SOCKET value = INVALID_SOCKET) noexcept
        : value_{value}
    {
    }

    ~UniqueSocket() noexcept
    {
        if (value_ != INVALID_SOCKET) {
            static_cast<void>(::closesocket(value_));
        }
    }

    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_SOCKET;
    }

private:
    SOCKET value_{INVALID_SOCKET};
};

class WinsockLifetime final {
public:
    WinsockLifetime()
    {
        WSADATA data{};
        require(
            ::WSAStartup(MAKEWORD(2, 2), &data) == 0,
            "WSAStartup failed for the Manager lifecycle fixture");
        started_ = true;
    }

    ~WinsockLifetime() noexcept
    {
        if (started_) {
            static_cast<void>(::WSACleanup());
        }
    }

    WinsockLifetime(const WinsockLifetime&) = delete;
    WinsockLifetime& operator=(const WinsockLifetime&) = delete;

private:
    bool started_{};
};

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    require(!value.empty(), "an empty UTF-16 value cannot be converted");
    require(
        value.size() <=
            static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "a UTF-16 value exceeds the conversion bound");
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    require(required > 0, "WideCharToMultiByte could not size a value");

    std::string result(static_cast<std::size_t>(required), '\0');
    require(
        ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr) == required,
        "WideCharToMultiByte could not convert a value");
    return result;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string_view value)
{
    require(!value.empty(), "an empty UTF-8 value cannot be converted");
    require(
        value.size() <=
            static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "a UTF-8 value exceeds the conversion bound");
    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    require(required > 0, "MultiByteToWideChar could not size a value");

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    require(
        ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) == required,
        "MultiByteToWideChar could not convert a value");
    return result;
}

[[nodiscard]] bool equalWindowsText(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
        ::CompareStringOrdinal(
            left.data(),
            static_cast<int>(left.size()),
            right.data(),
            static_cast<int>(right.size()),
            TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring canonicalPathForHandle(const HANDLE handle)
{
    const DWORD required = ::GetFinalPathNameByHandleW(
        handle,
        nullptr,
        0U,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    require(
        required > 0U && required <= MaximumPathCharacters + 4U,
        "GetFinalPathNameByHandleW could not size a canonical path");

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(required) + 1U,
        L'\0');
    const DWORD written = ::GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    require(
        written > 0U && written < buffer.size(),
        "GetFinalPathNameByHandleW could not resolve a canonical path");

    std::wstring canonical{buffer.data(), static_cast<std::size_t>(written)};
    constexpr std::wstring_view ExtendedPrefix{L"\\\\?\\"};
    constexpr std::wstring_view ExtendedUncPrefix{L"\\\\?\\UNC\\"};
    require(
        !canonical.starts_with(ExtendedUncPrefix),
        "the Manager lifecycle fixture requires a local-drive path");
    if (canonical.starts_with(ExtendedPrefix)) {
        canonical.erase(0U, ExtendedPrefix.size());
    }
    require(
        canonical.size() >= 3U && canonical[1U] == L':' &&
            canonical[2U] == L'\\',
        "a canonical fixture path is not a local DOS-drive path");
    return canonical;
}

[[nodiscard]] std::wstring_view leafName(const std::wstring_view path)
{
    const std::size_t separator = path.find_last_of(L'\\');
    require(
        separator != std::wstring_view::npos && separator + 1U < path.size(),
        "an executable path has no leaf name");
    return path.substr(separator + 1U);
}

[[nodiscard]] std::wstring_view parentPath(const std::wstring_view path)
{
    const std::size_t separator = path.find_last_of(L'\\');
    require(
        separator != std::wstring_view::npos && separator >= 2U,
        "an executable path has no parent directory");
    return path.substr(0U, separator);
}

[[nodiscard]] std::wstring canonicalRegularFile(
    const std::wstring& input,
    const std::wstring_view expectedLeaf)
{
    require(
        input.size() >= 3U && input[1U] == L':' &&
            (input[2U] == L'\\' || input[2U] == L'/') &&
            input.find(L'\0') == std::wstring::npos,
        "an executable argument is not an absolute local-drive path");
    UniqueHandle file{::CreateFileW(
        input.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    require(file.valid(), "an executable argument could not be opened");

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    require(
        ::GetFileInformationByHandleEx(
            file.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) != FALSE,
        "an executable argument could not be inspected");
    require(
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U,
        "an executable argument is not a regular non-reparse file");

    auto canonical = canonicalPathForHandle(file.get());
    require(
        leafName(canonical) == expectedLeaf,
        "an executable argument has the wrong required leaf name");
    return canonical;
}

void requireByteIdenticalFiles(
    const std::wstring& leftPath,
    const std::wstring& rightPath,
    const std::string_view description)
{
    UniqueHandle left{::CreateFileW(
        leftPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    UniqueHandle right{::CreateFileW(
        rightPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    require(
        left.valid() && right.valid(),
        std::string{description} + " could not be opened for an exact comparison");

    LARGE_INTEGER leftSize{};
    LARGE_INTEGER rightSize{};
    require(
        ::GetFileSizeEx(left.get(), &leftSize) != FALSE &&
            ::GetFileSizeEx(right.get(), &rightSize) != FALSE &&
            leftSize.QuadPart > 0 && leftSize.QuadPart == rightSize.QuadPart &&
            static_cast<std::uint64_t>(leftSize.QuadPart) <=
                MaximumQualifiedExecutableBytes,
        std::string{description} +
            " does not have one exact nonempty bounded byte length");

    std::array<std::byte, 64U * 1024U> leftBytes{};
    std::array<std::byte, 64U * 1024U> rightBytes{};
    std::uint64_t remaining = static_cast<std::uint64_t>(leftSize.QuadPart);
    while (remaining != 0U) {
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::uint64_t>(leftBytes.size())));
        DWORD leftRead{};
        DWORD rightRead{};
        require(
            ::ReadFile(
                left.get(), leftBytes.data(), requested, &leftRead, nullptr) != FALSE &&
                ::ReadFile(
                    right.get(), rightBytes.data(), requested, &rightRead, nullptr) != FALSE &&
                leftRead == requested && rightRead == requested &&
                std::equal(
                    leftBytes.begin(),
                    leftBytes.begin() + requested,
                    rightBytes.begin()),
            std::string{description} + " is not byte-identical");
        remaining -= requested;
    }
}

[[nodiscard]] bool pathIsMissing(const std::wstring& path) noexcept
{
    ::SetLastError(ERROR_SUCCESS);
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool pathWithinRoot(
    const std::wstring_view path,
    const std::wstring_view root) noexcept
{
    if (equalWindowsText(path, root)) {
        return true;
    }
    return path.size() > root.size() && path[root.size()] == L'\\' &&
        ::CompareStringOrdinal(
            path.data(),
            static_cast<int>(root.size()),
            root.data(),
            static_cast<int>(root.size()),
            TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool removeOwnedTreeEntry(
    const std::wstring& path,
    const std::wstring& root) noexcept
{
    try {
        if (!pathWithinRoot(path, root)) {
            return false;
        }

        ::SetLastError(ERROR_SUCCESS);
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = ::GetLastError();
            return error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND;
        }

        const bool directory =
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        const bool reparse =
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
        if (reparse) {
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
                static_cast<void>(::SetFileAttributesW(
                    path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY));
            }
            return directory
                ? ::RemoveDirectoryW(path.c_str()) != FALSE
                : ::DeleteFileW(path.c_str()) != FALSE;
        }

        if (!directory) {
            UniqueHandle file{::CreateFileW(
                path.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr)};
            if (!file.valid() ||
                !pathWithinRoot(canonicalPathForHandle(file.get()), root)) {
                return false;
            }
            file.reset();
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U &&
                ::SetFileAttributesW(
                    path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) ==
                    FALSE) {
                return false;
            }
            return ::DeleteFileW(path.c_str()) != FALSE;
        }

        UniqueHandle directoryHandle{::CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!directoryHandle.valid() ||
            !pathWithinRoot(
                canonicalPathForHandle(directoryHandle.get()), root)) {
            return false;
        }

        std::wstring pattern{path};
        pattern += L"\\*";
        WIN32_FIND_DATAW item{};
        UniqueFindHandle search{::FindFirstFileW(pattern.c_str(), &item)};
        if (search.valid()) {
            do {
                const std::wstring_view leaf{item.cFileName};
                if (leaf == L"." || leaf == L"..") {
                    continue;
                }
                if (leaf.empty() || leaf.find(L'\\') != std::wstring_view::npos ||
                    leaf.find(L'/') != std::wstring_view::npos ||
                    leaf.find(L'\0') != std::wstring_view::npos) {
                    return false;
                }
                std::wstring child{path};
                child.push_back(L'\\');
                child.append(leaf);
                if (child.size() > MaximumPathCharacters ||
                    !removeOwnedTreeEntry(child, root)) {
                    return false;
                }
            } while (::FindNextFileW(search.get(), &item) != FALSE);
            if (::GetLastError() != ERROR_NO_MORE_FILES) {
                return false;
            }
        } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
            return false;
        }

        search.reset();
        directoryHandle.reset();
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
            static_cast<void>(::SetFileAttributesW(
                path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY));
        }
        return ::RemoveDirectoryW(path.c_str()) != FALSE;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::wstring reserveTemporaryRoot(const std::wstring& suffix)
{
    std::array<wchar_t, MaximumPathCharacters + 1U> temporary{};
    const DWORD length = ::GetTempPathW(
        static_cast<DWORD>(temporary.size()),
        temporary.data());
    require(
        length > 0U && length < temporary.size(),
        "GetTempPathW could not resolve the fixture parent");

    std::wstring candidate{
        temporary.data(), static_cast<std::size_t>(length)};
    if (candidate.back() != L'\\') {
        candidate.push_back(L'\\');
    }
    candidate += L"ForgeConductor.Manager.Lifecycle.";
    candidate += suffix;
    require(
        candidate.size() <= MaximumPathCharacters,
        "the isolated Manager data root exceeds the path bound");
    if (::CreateDirectoryW(candidate.c_str(), nullptr) == FALSE) {
        fail(
            ::GetLastError() == ERROR_ALREADY_EXISTS
                ? "the isolated Manager data root already exists; refusing to reuse it"
                : "CreateDirectoryW could not reserve the isolated Manager data root");
    }

    try {
        UniqueHandle directory{::CreateFileW(
            candidate.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        require(
            directory.valid(),
            "the isolated Manager data root could not be opened");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        require(
            ::GetFileInformationByHandleEx(
                directory.get(),
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) != FALSE &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U,
            "the isolated Manager data root is not a regular directory");
        return canonicalPathForHandle(directory.get());
    } catch (...) {
        // The just-created directory is still empty. Remove only that exact
        // leaf and do not inspect or alter its temporary parent.
        static_cast<void>(::RemoveDirectoryW(candidate.c_str()));
        throw;
    }
}

[[nodiscard]] bool registryKeyExists(const std::wstring& subkey)
{
    HKEY opened{};
    const LSTATUS status = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        subkey.c_str(),
        0U,
        KEY_QUERY_VALUE,
        &opened);
    if (status == ERROR_SUCCESS) {
        static_cast<void>(::RegCloseKey(opened));
        return true;
    }
    require(
        status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND,
        "the isolated registry scope could not be inspected");
    return false;
}

[[nodiscard]] bool mutexNameIsFree(const std::wstring& mutexName)
{
    ::SetLastError(ERROR_SUCCESS);
    UniqueHandle mutex{::OpenMutexW(
        SYNCHRONIZE,
        FALSE,
        mutexName.c_str())};
    if (mutex.valid()) {
        return false;
    }
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

[[nodiscard]] bool pipeNameIsFree(const std::wstring& pipeName)
{
    UniqueHandle probe{::CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1U,
        4096U,
        4096U,
        0U,
        nullptr)};
    return probe.valid();
}

[[nodiscard]] bool dashboardPortIsFree()
{
    UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid()) {
        return false;
    }
    const BOOL exclusive = TRUE;
    if (::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive)) == SOCKET_ERROR) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(ExpectedDashboardPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::bind(
               socket.get(),
               reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != SOCKET_ERROR;
}

class IsolatedManagerEnvironment final {
public:
    explicit IsolatedManagerEnvironment(std::string purposeSuffix)
        : purposeSuffix_{std::move(purposeSuffix)},
          registrySubkey_{
              std::wstring{RegistryLeafPrefix} +
              utf8ToWide(purposeSuffix_)}
    {
        require(
            !purposeSuffix_.empty() &&
                purposeSuffix_.size() <=
                    Infrastructure::WindowsManagerInstanceLease::
                        MaximumPurposeSuffixCharacters,
            "the Manager lifecycle purpose suffix is outside its bound");

        const auto identity = take(
            Infrastructure::WindowsCurrentUserIdentity::load(),
            "load current-user identity");
        const auto names = take(
            Infrastructure::WindowsManagerInstanceLease::namesFor(
                identity,
                Infrastructure::WindowsManagerInstanceLeaseOptions{
                    purposeSuffix_}),
            "derive isolated Manager instance names");
        mutexName_ = names.mutexName();
        pipeName_ = names.pipeName();

        registryParentExisted_ =
            registryKeyExists(std::wstring{RegistryParent});
        require(
            !registryKeyExists(registrySubkey_),
            "the isolated Manager registry key already exists; refusing to overwrite it");
        require(
            mutexNameIsFree(mutexName_),
            "the isolated Manager singleton is already owned; refusing to disturb it");
        require(
            pipeNameIsFree(pipeName_),
            "the isolated Manager pipe is already owned; refusing to disturb it");
        require(
            dashboardPortIsFree(),
            "the dashboard port is already owned; refusing to disturb it");

        root_ = reserveTemporaryRoot(utf8ToWide(purposeSuffix_));
    }

    ~IsolatedManagerEnvironment() noexcept
    {
        if (!cleanup()) {
            std::cerr
                << "[CLEANUP FAILURE] isolated Manager root or registry residue remains\n";
        }
    }

    IsolatedManagerEnvironment(const IsolatedManagerEnvironment&) = delete;
    IsolatedManagerEnvironment& operator=(
        const IsolatedManagerEnvironment&) = delete;

    [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
    [[nodiscard]] const std::string& purposeSuffix() const noexcept
    {
        return purposeSuffix_;
    }
    [[nodiscard]] const std::wstring& registrySubkey() const noexcept
    {
        return registrySubkey_;
    }
    [[nodiscard]] const std::wstring& mutexName() const noexcept
    {
        return mutexName_;
    }
    [[nodiscard]] const std::wstring& pipeName() const noexcept
    {
        return pipeName_;
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        if (cleaned_) {
            return true;
        }
        try {
            // A live purpose-scoped Manager owns both names. Refuse to mutate
            // its isolated state if exact process cleanup did not complete.
            if (!mutexNameIsFree(mutexName_) ||
                !pipeNameIsFree(pipeName_)) {
                return false;
            }

            const LSTATUS removed = ::RegDeleteTreeW(
                HKEY_CURRENT_USER,
                registrySubkey_.c_str());
            if (removed != ERROR_SUCCESS &&
                removed != ERROR_FILE_NOT_FOUND &&
                removed != ERROR_PATH_NOT_FOUND) {
                return false;
            }
            if (registryKeyExists(registrySubkey_)) {
                return false;
            }

            if (!removeOwnedTreeEntry(root_, root_) ||
                !pathIsMissing(root_)) {
                return false;
            }

            if (!registryParentExisted_) {
                // Delete only an empty parent that this run may have caused
                // RegCreateKeyExW to materialize. ERROR_ACCESS_DENIED means
                // another owner placed data there; never recurse into it.
                const LSTATUS parentRemoval = ::RegDeleteKeyW(
                    HKEY_CURRENT_USER,
                    std::wstring{RegistryParent}.c_str());
                if (parentRemoval != ERROR_SUCCESS &&
                    parentRemoval != ERROR_FILE_NOT_FOUND &&
                    parentRemoval != ERROR_PATH_NOT_FOUND &&
                    parentRemoval != ERROR_ACCESS_DENIED) {
                    return false;
                }
            }

            cleaned_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::string purposeSuffix_;
    std::wstring registrySubkey_;
    std::wstring root_;
    std::wstring mutexName_;
    std::wstring pipeName_;
    bool registryParentExisted_{};
    bool cleaned_{};
};

[[nodiscard]] std::wstring quotedArgument(const std::wstring_view value)
{
    require(
        !value.empty() && value.find(L'\0') == std::wstring_view::npos &&
            value.find(L'"') == std::wstring_view::npos &&
            value.back() != L'\\',
        "a process argument cannot be quoted without ambiguity");
    std::wstring quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back(L'"');
    quoted.append(value);
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] DWORD boundedWaitMilliseconds(
    const std::chrono::milliseconds timeout) noexcept
{
    return static_cast<DWORD>((std::min)(
        timeout.count(),
        static_cast<std::int64_t>((std::numeric_limits<DWORD>::max)())));
}

[[nodiscard]] std::chrono::milliseconds remainingWait(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0ms;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
}

[[nodiscard]] bool terminateProcessAndWaitNoThrow(
    const HANDLE process,
    const DWORD exitCode,
    const std::chrono::milliseconds timeout) noexcept
{
    return process != nullptr && process != INVALID_HANDLE_VALUE &&
        ::TerminateProcess(process, exitCode) != FALSE &&
        ::WaitForSingleObject(
            process, boundedWaitMilliseconds(timeout)) == WAIT_OBJECT_0;
}

[[nodiscard]] std::optional<bool> jobHasActiveProcesses(
    const HANDLE job) noexcept
{
    if (job == nullptr || job == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (::QueryInformationJobObject(
            job,
            JobObjectBasicAccountingInformation,
            &accounting,
            sizeof(accounting),
            nullptr) == FALSE) {
        return std::nullopt;
    }
    return accounting.ActiveProcesses != 0U;
}

[[nodiscard]] bool waitForJobEmptyNoThrow(
    const HANDLE job,
    const std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto active = jobHasActiveProcesses(job);
        if (!active.has_value()) {
            return false;
        }
        if (!*active) {
            return true;
        }
        const auto remaining = remainingWait(deadline);
        if (remaining == 0ms) {
            return false;
        }
        std::this_thread::sleep_for((std::min)(PollInterval, remaining));
    }
}

[[nodiscard]] bool terminateJobAndWaitNoThrow(
    const HANDLE job,
    const HANDLE rootProcess,
    const DWORD exitCode,
    const std::chrono::milliseconds timeout) noexcept
{
    if (job == nullptr || job == INVALID_HANDLE_VALUE ||
        rootProcess == nullptr || rootProcess == INVALID_HANDLE_VALUE ||
        ::TerminateJobObject(job, exitCode) == FALSE) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool rootExited = ::WaitForSingleObject(
        rootProcess, boundedWaitMilliseconds(timeout)) == WAIT_OBJECT_0;
    const bool jobEmpty = rootExited &&
        waitForJobEmptyNoThrow(job, remainingWait(deadline));
    return rootExited && jobEmpty;
}

class OwnedManagerProcess final {
public:
    [[nodiscard]] static std::unique_ptr<OwnedManagerProcess> launch(
        const std::wstring& managerExecutable,
        const IsolatedManagerEnvironment& environment)
    {
        std::wstring commandLine = quotedArgument(managerExecutable);
        commandLine.push_back(L' ');
        commandLine += quotedArgument(environment.root());
        commandLine.push_back(L' ');
        commandLine += quotedArgument(
            utf8ToWide(environment.purposeSuffix()));
        commandLine.push_back(L' ');
        commandLine += quotedArgument(environment.registrySubkey());
        require(
            commandLine.size() < 32U * 1024U,
            "the Manager composition fixture command line exceeds its bound");
        std::vector<wchar_t> writable{
            commandLine.begin(), commandLine.end()};
        writable.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring workingDirectory{parentPath(managerExecutable)};
        UniqueHandle job{::CreateJobObjectW(nullptr, nullptr)};
        if (!job.valid()) {
            const DWORD error = ::GetLastError();
            fail(
                "CreateJobObjectW could not create the owned Manager process job; Win32 error " +
                std::to_string(error));
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(
                job.get(),
                JobObjectExtendedLimitInformation,
                &jobLimits,
                sizeof(jobLimits)) == FALSE) {
            const DWORD error = ::GetLastError();
            fail(
                "SetInformationJobObject could not bind kill-on-close ownership; Win32 error " +
                std::to_string(error));
        }
        if (::CreateProcessW(
                managerExecutable.c_str(),
                writable.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED |
                    CREATE_UNICODE_ENVIRONMENT,
                nullptr,
                workingDirectory.c_str(),
                &startup,
                &process) == FALSE) {
            const DWORD error = ::GetLastError();
            fail(
                "CreateProcessW could not launch the owned Manager composition fixture; Win32 error " +
                std::to_string(error));
        }

        UniqueHandle processHandle{process.hProcess};
        UniqueHandle threadHandle{process.hThread};
        if (process.dwProcessId == 0U || !processHandle.valid() ||
            !threadHandle.valid()) {
            const bool cleaned = processHandle.valid() &&
                terminateProcessAndWaitNoThrow(
                    processHandle.get(),
                    ForcedTerminationExitCode,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        CleanupProcessExitTimeout));
            fail(
                "CreateProcessW returned an incomplete owned process identity; suspended-child cleanup " +
                std::string{cleaned ? "completed" : "was not confirmed"});
        }
        if (::AssignProcessToJobObject(
                job.get(), processHandle.get()) == FALSE) {
            const DWORD error = ::GetLastError();
            const bool cleaned = terminateProcessAndWaitNoThrow(
                processHandle.get(),
                ForcedTerminationExitCode,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    CleanupProcessExitTimeout));
            fail(
                "AssignProcessToJobObject could not bind the exact Manager child; Win32 error " +
                std::to_string(error) + "; suspended-child cleanup " +
                std::string{cleaned ? "completed" : "was not confirmed"});
        }
        if (::ResumeThread(threadHandle.get()) ==
            (std::numeric_limits<DWORD>::max)()) {
            const DWORD error = ::GetLastError();
            const bool cleaned = terminateJobAndWaitNoThrow(
                job.get(),
                processHandle.get(),
                ForcedTerminationExitCode,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    CleanupProcessExitTimeout));
            fail(
                "ResumeThread could not start the exact job-bound Manager child; Win32 error " +
                std::to_string(error) + "; job cleanup " +
                std::string{cleaned ? "completed" : "was not confirmed"});
        }
        return std::unique_ptr<OwnedManagerProcess>{
            new OwnedManagerProcess{
                std::move(job),
                std::move(processHandle),
                process.dwProcessId}};
    }

    ~OwnedManagerProcess() noexcept
    {
        if (!job_.valid() || !process_.valid()) {
            return;
        }
        const auto active = jobHasActiveProcesses(job_.get());
        if (!active.has_value()) {
            std::cerr
                << "[CLEANUP FAILURE] the exact owned Manager job could not be inspected\n";
        } else if (
            *active &&
            !terminateJobAndWaitNoThrow(
                job_.get(),
                process_.get(),
                ForcedTerminationExitCode,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    CleanupProcessExitTimeout))) {
            std::cerr
                << "[CLEANUP FAILURE] the exact owned Manager process tree did not terminate\n";
        }
    }

    OwnedManagerProcess(const OwnedManagerProcess&) = delete;
    OwnedManagerProcess& operator=(const OwnedManagerProcess&) = delete;

    [[nodiscard]] DWORD processId() const noexcept { return processId_; }

    [[nodiscard]] bool running() const
    {
        const DWORD wait = ::WaitForSingleObject(process_.get(), 0U);
        require(
            wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT,
            "WaitForSingleObject failed for an exact owned Manager process");
        return wait == WAIT_TIMEOUT;
    }

    [[nodiscard]] DWORD exitCode() const
    {
        require(!running(), "an owned Manager process is still active");
        DWORD code{STILL_ACTIVE};
        require(
            ::GetExitCodeProcess(process_.get(), &code) != FALSE &&
                code != STILL_ACTIVE,
            "GetExitCodeProcess could not read a completed Manager process");
        return code;
    }

    [[nodiscard]] DWORD waitForExit(
        const std::chrono::milliseconds timeout) const
    {
        require(
            timeout > 0ms &&
                timeout.count() <=
                    static_cast<std::int64_t>(
                        (std::numeric_limits<DWORD>::max)()),
            "an owned process wait is outside the Win32 bound");
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        require(
            ::WaitForSingleObject(
                process_.get(),
                static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0,
            "an owned Manager process did not exit before its deadline");
        require(
            waitForJobEmptyNoThrow(job_.get(), remainingWait(deadline)),
            "an owned Manager process exited without draining its exact job tree");
        return exitCode();
    }

    [[nodiscard]] DWORD forceTerminate()
    {
        require(
            running(),
            "the forced-termination target exited before its owned handle was terminated");
        require(
            terminateJobAndWaitNoThrow(
                job_.get(),
                process_.get(),
                ForcedTerminationExitCode,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    ProcessExitTimeout)),
            "TerminateJobObject did not drain the exact owned Manager process tree");
        return exitCode();
    }

private:
    OwnedManagerProcess(
        UniqueHandle job,
        UniqueHandle process,
        const DWORD processId) noexcept
        : job_{std::move(job)},
          process_{std::move(process)},
          processId_{processId}
    {
    }

    UniqueHandle job_;
    UniqueHandle process_;
    DWORD processId_{};
};

class OperationSequence final {
public:
    [[nodiscard]] Domain::OperationContext next(
        const std::chrono::milliseconds lifetime = 5s)
    {
        require(
            lifetime > 0ms && lifetime <= 1min,
            "an operation lifetime is outside the lifecycle-test bound");
        std::string operation{"70000000-0000-4000-8000-000000000000"};
        auto remaining = ++counter_;
        constexpr char Digits[] = "0123456789abcdef";
        for (std::size_t index = 0U; index < 8U; ++index) {
            operation[operation.size() - 1U - index] =
                Digits[remaining & 0xFU];
            remaining >>= 4U;
        }
        return Domain::OperationContext{
            take(Domain::OperationId::parse(operation)),
            std::chrono::steady_clock::now() + lifetime,
            std::stop_token{},
            take(Domain::CorrelationId::parse(
                "manager-composition-lifecycle-" +
                std::to_string(counter_)))};
    }

private:
    std::uint32_t counter_{};
};

[[nodiscard]] Manager::ManagerTransportLimits lifecycleTransportLimits()
{
    Manager::ManagerTransportLimits limits;
    limits.maximumRequestLifetime = 10s;
    limits.connectTimeout = 250ms;
    limits.shutdownDrainTimeout = 2s;
    limits.maximumConcurrentClientRequests = 4U;
    limits.maximumActiveRegularOperations = 3U;
    return limits;
}

void requireReadyStatus(
    const Domain::ManagerStatus& status,
    const OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    const std::string_view stage)
{
    const std::string prefix{stage};
    require(
        status.ok && status.isManager &&
            status.state == Domain::ManagerServiceState::Running &&
            status.desiredRunning && status.httpListening &&
            status.serviceActive,
        prefix + " did not report a fully running Manager and dashboard");
    require(
        status.processId == process.processId(),
        prefix + " did not report the exact owned process ID");
    require(
        !status.openBrowserOnStart,
        prefix + " did not retain the isolated no-browser configuration");
    require(
        status.dashboardHost == "127.0.0.1" &&
            status.dashboardPort == ExpectedDashboardPort,
        prefix + " did not own the exact loopback dashboard endpoint");
    require(
        equalWindowsText(
            utf8ToWide(status.home.value()), environment.root()),
        prefix + " did not report the exact isolated home");
    require(
        !status.version.empty(),
        prefix + " did not report a product version");
}

struct ReadyManager final {
    Domain::Sha256Digest nonce;
    std::unique_ptr<Infrastructure::WindowsManagerNamedPipeClient> client;
    Domain::ManagerStatus status;
};

[[nodiscard]] ReadyManager waitForReady(
    OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    Infrastructure::WindowsManagerAuthenticationTokenStore& tokenStore,
    const std::shared_ptr<Infrastructure::SystemClock>& clock,
    OperationSequence& operations,
    const std::string_view stage)
{
    const auto deadline = std::chrono::steady_clock::now() + StartupTimeout;
    std::string lastObservation{"the isolated credentials are not published"};
    do {
        if (!process.running()) {
            fail(
                std::string{stage} +
                " exited before readiness with code " +
                std::to_string(process.exitCode()));
        }

        auto loaded = tokenStore.load(operations.next(2s));
        if (!loaded) {
            lastObservation = loaded.error().code + ": " +
                loaded.error().message;
        } else if (!loaded.value().has_value()) {
            lastObservation =
                "the isolated Manager authentication token is not yet present";
        } else {
            Domain::Sha256Digest nonce = loaded.value().value();
            auto created = Infrastructure::WindowsManagerNamedPipeClient::create(
                clock,
                environment.pipeName(),
                nonce,
                lifecycleTransportLimits());
            if (!created) {
                lastObservation = created.error().code + ": " +
                    created.error().message;
            } else {
                auto client = std::move(created).value();
                auto observed = client->status(operations.next(3s));
                if (observed) {
                    auto status = std::move(observed).value();
                    requireReadyStatus(
                        status, process, environment, stage);
                    return ReadyManager{
                        std::move(nonce),
                        std::move(client),
                        std::move(status)};
                }
                lastObservation = observed.error().code + ": " +
                    observed.error().message;
                client->shutdown();
            }
        }

        std::this_thread::sleep_for(PollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    fail(
        std::string{stage} +
        " did not become ready before its deadline; last observation: " +
        lastObservation);
}

[[nodiscard]] Domain::Sha256Digest wrongNonce(
    const Domain::Sha256Digest& valid)
{
    std::string changed = valid.value();
    require(changed.size() == 64U, "the valid nonce has the wrong size");
    changed.front() = changed.front() == '0' ? '1' : '0';
    return take(Domain::Sha256Digest::parse(changed));
}

void proveWrongNonceAndRecovery(
    ReadyManager& ready,
    OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    Infrastructure::WindowsManagerAuthenticationTokenStore& tokenStore,
    const std::shared_ptr<Infrastructure::SystemClock>& clock,
    OperationSequence& operations)
{
    const std::uint32_t restartCountBeforeRejection =
        ready.status.restartCount;
    auto wrongClient = take(
        Infrastructure::WindowsManagerNamedPipeClient::create(
            clock,
            environment.pipeName(),
            wrongNonce(ready.nonce),
            lifecycleTransportLimits()),
        "create wrong-nonce Manager client");
    const auto rejected = wrongClient->status(operations.next(5s));
    require(
        !rejected &&
            rejected.error().code == Domain::ErrorCodes::Unauthorized,
        "the isolated Manager did not reject the wrong authentication nonce");
    const auto rejectedRestart =
        wrongClient->requestRestart(operations.next(5s));
    require(
        !rejectedRestart &&
            rejectedRestart.error().code == Domain::ErrorCodes::Unauthorized,
        "the isolated Manager restart command did not reject the wrong authentication nonce");
    wrongClient->shutdown();

    const auto recovered = take(
        ready.client->status(operations.next(5s)),
        "authenticated status after wrong-nonce rejection");
    requireReadyStatus(
        recovered,
        process,
        environment,
        "wrong-nonce recovery");
    require(
        recovered.restartCount == restartCountBeforeRejection,
        "the rejected wrong-nonce restart changed the Manager generation");

    const auto reloaded = take(
        tokenStore.load(operations.next(5s)),
        "reload isolated Manager authentication token");
    require(
        reloaded.has_value() && reloaded.value() == ready.nonce,
        "wrong-nonce rejection changed the isolated Manager credential");
    ready.status = recovered;
}

void proveExplicitRestartAndRecovery(
    ReadyManager& ready,
    OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    Infrastructure::WindowsManagerAuthenticationTokenStore& tokenStore,
    OperationSequence& operations)
{
    require(
        ready.status.restartCount <
            (std::numeric_limits<std::uint32_t>::max)(),
        "the isolated Manager restart generation is exhausted");
    const std::uint32_t expectedRestartCount =
        ready.status.restartCount + 1U;
    requireSuccess(
        ready.client->requestRestart(operations.next(10s)),
        "receive authenticated Manager restart acknowledgement");

    const auto deadline = std::chrono::steady_clock::now() + StartupTimeout;
    std::string lastObservation{
        "the restarted Manager has not published a status snapshot"};
    do {
        require(
            process.running(),
            "the explicit runtime restart terminated the Manager process");
        auto observed = ready.client->status(operations.next(3s));
        if (!observed) {
            lastObservation = observed.error().code + ": " +
                observed.error().message;
        } else {
            auto status = std::move(observed).value();
            require(
                status.restartCount <= expectedRestartCount,
                "the explicit runtime restart advanced more than one generation");
            if (status.restartCount == expectedRestartCount &&
                status.state == Domain::ManagerServiceState::Running) {
                requireReadyStatus(
                    status,
                    process,
                    environment,
                    "explicit restart recovery");
                const auto reloaded = take(
                    tokenStore.load(operations.next(5s)),
                    "reload credential after explicit Manager restart");
                require(
                    reloaded.has_value() && reloaded.value() == ready.nonce,
                    "the explicit Manager restart changed the isolated credential");
                ready.status = std::move(status);
                return;
            }
            lastObservation =
                "state=" +
                std::to_string(static_cast<int>(status.state)) +
                ", restart_count=" +
                std::to_string(status.restartCount);
        }
        std::this_thread::sleep_for(PollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    fail(
        "the explicit Manager restart did not recover before its deadline; "
        "last observation: " +
        lastObservation);
}

void proveSecondInstanceRejected(
    const std::wstring& managerExecutable,
    const IsolatedManagerEnvironment& environment,
    ReadyManager& firstReady,
    OwnedManagerProcess& first,
    Infrastructure::WindowsManagerAuthenticationTokenStore& tokenStore,
    OperationSequence& operations)
{
    auto second = OwnedManagerProcess::launch(
        managerExecutable, environment);
    require(
        second->processId() != first.processId(),
        "CreateProcessW reused the live first Manager process identity");
    const DWORD secondExit = second->waitForExit(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ProcessExitTimeout));
    require(
        secondExit == FixtureOwnershipConflictExitCode,
        "the second Manager fixture did not expose the typed ownership-conflict result");
    require(
        first.running(),
        "the rejected second Manager instance disturbed the first process");

    const auto afterConflict = take(
        firstReady.client->status(operations.next(5s)),
        "authenticated status after second-instance rejection");
    requireReadyStatus(
        afterConflict,
        first,
        environment,
        "second-instance rejection recovery");
    const auto reloaded = take(
        tokenStore.load(operations.next(5s)),
        "reload credential after second-instance rejection");
    require(
        reloaded.has_value() && reloaded.value() == firstReady.nonce,
        "the rejected second instance changed the isolated credential");
}

void waitForInfrastructureReleased(
    const IsolatedManagerEnvironment& environment,
    const std::string_view stage)
{
    const auto deadline =
        std::chrono::steady_clock::now() + InfrastructureReleaseTimeout;
    do {
        if (mutexNameIsFree(environment.mutexName()) &&
            pipeNameIsFree(environment.pipeName()) &&
            dashboardPortIsFree()) {
            return;
        }
        std::this_thread::sleep_for(PollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    fail(
        std::string{stage} +
        " left singleton, pipe, or loopback-port residue after process exit");
}

void shutdownThroughManagerPipe(
    ReadyManager& ready,
    OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    OperationSequence& operations)
{
    requireSuccess(
        ready.client->requestShutdown(operations.next(10s)),
        "receive graceful Manager pipe shutdown acknowledgement");
    const DWORD exitCode = process.waitForExit(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ProcessExitTimeout));
    ready.client->shutdown();
    require(
        exitCode == EXIT_SUCCESS,
        "the pipe-shutdown Manager did not exit successfully");
    waitForInfrastructureReleased(environment, "Manager pipe shutdown");
}

[[nodiscard]] std::string postDashboardShutdown(
    const Domain::Sha256Digest& bearer)
{
    UniqueInternetHandle session{::WinHttpOpen(
        L"ForgeConductor-Manager-Lifecycle/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0U)};
    require(
        static_cast<bool>(session),
        "WinHttpOpen failed for the isolated dashboard shutdown request");
    require(
        ::WinHttpSetTimeouts(
            session.get(), 3000, 3000, 3000, 5000) != FALSE,
        "WinHttpSetTimeouts failed for the isolated dashboard request");

    UniqueInternetHandle connection{::WinHttpConnect(
        session.get(),
        L"127.0.0.1",
        ExpectedDashboardPort,
        0U)};
    require(
        static_cast<bool>(connection),
        "WinHttpConnect failed for the isolated dashboard endpoint");

    const wchar_t* acceptedTypes[]{L"application/json", nullptr};
    UniqueInternetHandle request{::WinHttpOpenRequest(
        connection.get(),
        L"POST",
        L"/api/manager/shutdown",
        nullptr,
        WINHTTP_NO_REFERER,
        acceptedTypes,
        WINHTTP_FLAG_REFRESH)};
    require(
        static_cast<bool>(request),
        "WinHttpOpenRequest failed for the dashboard shutdown route");

    std::wstring headers{L"Authorization: Bearer "};
    headers += utf8ToWide(bearer.value());
    headers += L"\r\nContent-Type: application/json";
    headers += L"\r\nOrigin: http://127.0.0.1:";
    headers += std::to_wstring(ExpectedDashboardPort);
    headers += L"\r\nSec-Fetch-Site: same-origin\r\n";
    constexpr std::array<char, 2U> Body{'{', '}'};
    require(
        ::WinHttpSendRequest(
            request.get(),
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            const_cast<char*>(Body.data()),
            static_cast<DWORD>(Body.size()),
            static_cast<DWORD>(Body.size()),
            0U) != FALSE,
        "the authenticated dashboard shutdown request could not be sent");
    require(
        ::WinHttpReceiveResponse(request.get(), nullptr) != FALSE,
        "the dashboard shutdown acknowledgement headers were not received");

    DWORD status{};
    DWORD statusBytes = sizeof(status);
    require(
        ::WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusBytes,
            WINHTTP_NO_HEADER_INDEX) != FALSE &&
            status == 200U,
        "the dashboard shutdown route did not return HTTP 200");

    constexpr std::size_t MaximumAcknowledgementBytes = 4096U;
    std::string response;
    for (;;) {
        DWORD available{};
        require(
            ::WinHttpQueryDataAvailable(request.get(), &available) != FALSE,
            "the dashboard shutdown response body could not be sized");
        if (available == 0U) {
            break;
        }
        require(
            response.size() + static_cast<std::size_t>(available) <=
                MaximumAcknowledgementBytes,
            "the dashboard shutdown acknowledgement exceeded its bound");
        std::vector<char> chunk(static_cast<std::size_t>(available));
        DWORD read{};
        require(
            ::WinHttpReadData(
                request.get(), chunk.data(), available, &read) != FALSE &&
                read > 0U && read <= available,
            "the dashboard shutdown response body could not be read");
        response.append(chunk.data(), static_cast<std::size_t>(read));
    }
    return response;
}

void shutdownThroughDashboard(
    ReadyManager& ready,
    OwnedManagerProcess& process,
    const IsolatedManagerEnvironment& environment,
    Infrastructure::WindowsDashboardBearerTokenStore& bearerStore,
    OperationSequence& operations)
{
    const auto bearer = take(
        bearerStore.load(operations.next(5s)),
        "load isolated dashboard bearer");
    require(
        bearer.has_value(),
        "the isolated dashboard bearer was not persisted");
    const std::string acknowledgement =
        postDashboardShutdown(bearer.value());
    require(
        acknowledgement == ExpectedShutdownAcknowledgement,
        "the dashboard shutdown acknowledgement body was not exact");

    const DWORD exitCode = process.waitForExit(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ProcessExitTimeout));
    ready.client->shutdown();
    require(
        exitCode == EXIT_SUCCESS,
        "the dashboard-shutdown Manager did not exit successfully");
    waitForInfrastructureReleased(environment, "dashboard shutdown");
}

[[nodiscard]] std::string uniquePurposeSuffix()
{
    return "p16-" + std::to_string(::GetCurrentProcessId()) + '-' +
        std::to_string(::GetTickCount64());
}

void runLifecycle(
    const std::wstring& fixtureArgument,
    const std::wstring& canonicalCliArgument)
{
    WinsockLifetime winsock;
    const std::wstring managerExecutable = canonicalRegularFile(
        fixtureArgument, L"ForgeConductor.Manager.exe");
    std::wstring expectedCli{parentPath(managerExecutable)};
    expectedCli += L"\\forge-conductor.exe";
    const std::wstring cliExecutable = canonicalRegularFile(
        expectedCli, L"forge-conductor.exe");
    const std::wstring canonicalCliExecutable = canonicalRegularFile(
        canonicalCliArgument, L"forge-conductor.exe");
    require(
        equalWindowsText(
            parentPath(managerExecutable), parentPath(cliExecutable)) &&
            equalWindowsText(expectedCli, cliExecutable),
        "the controlled Manager fixture does not have its exact staged sibling CLI");
    requireByteIdenticalFiles(
        cliExecutable,
        canonicalCliExecutable,
        "the controlled Manager fixture sibling and canonical CLI");

    IsolatedManagerEnvironment environment{uniquePurposeSuffix()};
    Infrastructure::DpapiSecureStorage secureStorage{
        environment.registrySubkey()};
    Infrastructure::WindowsManagerAuthenticationTokenGenerator
        managerTokenGenerator;
    Infrastructure::WindowsDashboardBearerTokenGenerator
        dashboardBearerGenerator;
    Infrastructure::WindowsManagerAuthenticationTokenStore managerTokenStore{
        secureStorage, managerTokenGenerator};
    Infrastructure::WindowsDashboardBearerTokenStore dashboardBearerStore{
        secureStorage, dashboardBearerGenerator};
    auto clock = std::make_shared<Infrastructure::SystemClock>();
    OperationSequence operations;

    auto first = OwnedManagerProcess::launch(
        managerExecutable, environment);
    auto firstReady = waitForReady(
        *first,
        environment,
        managerTokenStore,
        clock,
        operations,
        "first Manager instance");
    const Domain::Sha256Digest stableNonce = firstReady.nonce;
    proveSecondInstanceRejected(
        managerExecutable,
        environment,
        firstReady,
        *first,
        managerTokenStore,
        operations);
    proveWrongNonceAndRecovery(
        firstReady,
        *first,
        environment,
        managerTokenStore,
        clock,
        operations);
    proveExplicitRestartAndRecovery(
        firstReady,
        *first,
        environment,
        managerTokenStore,
        operations);
    shutdownThroughManagerPipe(
        firstReady, *first, environment, operations);

    auto forced = OwnedManagerProcess::launch(
        managerExecutable, environment);
    auto forcedReady = waitForReady(
        *forced,
        environment,
        managerTokenStore,
        clock,
        operations,
        "forced-exit Manager instance");
    require(
        forcedReady.nonce == stableNonce,
        "the forced-exit instance did not reuse the exact isolated credential");
    require(
        forcedReady.status.processId != firstReady.status.processId,
        "the forced-exit cycle did not publish a new process identity");
    const DWORD forcedExit = forced->forceTerminate();
    forcedReady.client->shutdown();
    require(
        forcedExit == ForcedTerminationExitCode,
        "the exact owned forced-exit child reported the wrong termination code");
    waitForInfrastructureReleased(environment, "forced Manager termination");

    auto successor = OwnedManagerProcess::launch(
        managerExecutable, environment);
    auto successorReady = waitForReady(
        *successor,
        environment,
        managerTokenStore,
        clock,
        operations,
        "successor Manager instance");
    require(
        successorReady.nonce == stableNonce,
        "the exact successor did not reuse the isolated credential");
    require(
        successorReady.status.processId != firstReady.status.processId &&
            successorReady.status.processId != forcedReady.status.processId,
        "the exact successor did not publish a distinct process identity");
    shutdownThroughDashboard(
        successorReady,
        *successor,
        environment,
        dashboardBearerStore,
        operations);

    require(
        !first->running() && first->exitCode() == EXIT_SUCCESS &&
            !forced->running() &&
            forced->exitCode() == ForcedTerminationExitCode &&
            !successor->running() &&
            successor->exitCode() == EXIT_SUCCESS,
        "one or more exact owned Manager processes remained active or exited unexpectedly");

    managerTokenStore.shutdown();
    dashboardBearerStore.shutdown();
    secureStorage.shutdown();
    require(
        mutexNameIsFree(environment.mutexName()) &&
            pipeNameIsFree(environment.pipeName()) &&
            dashboardPortIsFree(),
        "owned Manager infrastructure remained before exact state cleanup");
    require(
        environment.cleanup(),
        "the exact isolated Manager registry or data root could not be cleaned");
    require(
        !registryKeyExists(environment.registrySubkey()) &&
            pathIsMissing(environment.root()) &&
            mutexNameIsFree(environment.mutexName()) &&
            pipeNameIsFree(environment.pipeName()) &&
            dashboardPortIsFree(),
        "the Manager composition lifecycle left owned residue");
}

} // namespace

int wmain(const int argc, wchar_t** const argv)
{
    if (argc != 3 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr) {
        std::cerr
            << "Usage: manager-composition-lifecycle-tests "
               "<absolute-ForgeConductor.Manager.exe-fixture> "
               "<absolute-canonical-forge-conductor.exe>\n";
        return 2;
    }

    try {
        runLifecycle(argv[1], argv[2]);
        std::cout
            << "[PASS] manager_composition.real_process.lifecycle\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "[FAIL] manager_composition.real_process.lifecycle: "
            << error.what() << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr
            << "[FAIL] manager_composition.real_process.lifecycle: unknown native failure\n";
        return EXIT_FAILURE;
    }
}
