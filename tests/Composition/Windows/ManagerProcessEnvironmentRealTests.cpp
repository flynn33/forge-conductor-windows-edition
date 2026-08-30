#include "ManagerProcessEnvironment.h"

#include "ForgeConductor/Domain/ResourcePolicy.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;

using namespace std::chrono_literals;

constexpr std::size_t MaximumPathCharacters = 32U * 1024U;
constexpr auto OperationTimeout = 30s;
constexpr std::array<std::wstring_view, 4U> ExpectedChildLeaves{
    L"config", L"exports", L"logs", L"projects"};

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

class UniqueHandle final {
public:
    explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueHandle() noexcept
    {
        if (valid()) {
            static_cast<void>(::CloseHandle(value_));
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&&) = delete;
    UniqueHandle& operator=(UniqueHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
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
        if (value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::FindClose(value_));
        }
    }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
    UniqueFindHandle(UniqueFindHandle&&) = delete;
    UniqueFindHandle& operator=(UniqueFindHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    require(!value.empty(), "an empty Windows path cannot be converted");
    require(
        value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "the Windows path exceeds the UTF-8 conversion bound");
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0, "WideCharToMultiByte could not size a path");

    std::string result(static_cast<std::size_t>(required), '\0');
    require(
        ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required, nullptr,
            nullptr) == required,
        "WideCharToMultiByte could not convert a path");
    return result;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string_view value)
{
    require(!value.empty(), "an empty UTF-8 path cannot be converted");
    require(
        value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "the UTF-8 path exceeds the Windows conversion bound");
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    require(required > 0, "MultiByteToWideChar could not size a path");

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    require(
        ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) ==
            required,
        "MultiByteToWideChar could not convert a path");
    return result;
}

[[nodiscard]] Domain::PathText pathText(const std::wstring_view value)
{
    return take(Domain::PathText::create(wideToUtf8(value)));
}

[[nodiscard]] bool equalWindowsPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
        ::CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()), right.data(),
            static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring canonicalPathForHandle(const HANDLE handle)
{
    const DWORD required = ::GetFinalPathNameByHandleW(
        handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    require(
        required > 0U && required <= MaximumPathCharacters + 4U,
        "GetFinalPathNameByHandleW could not size a canonical path");

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    require(
        written > 0U && written < buffer.size(),
        "GetFinalPathNameByHandleW could not resolve a canonical path");

    std::wstring canonical{buffer.data(), static_cast<std::size_t>(written)};
    constexpr std::wstring_view ExtendedPrefix{L"\\\\?\\"};
    constexpr std::wstring_view ExtendedUncPrefix{L"\\\\?\\UNC\\"};
    require(
        !canonical.starts_with(ExtendedUncPrefix),
        "the real-process fixture requires a local-drive path");
    if (canonical.starts_with(ExtendedPrefix)) {
        canonical.erase(0U, ExtendedPrefix.size());
    }
    require(
        canonical.size() >= 3U && canonical[1U] == L':' &&
            canonical[2U] == L'\\',
        "the canonical fixture path is not a local DOS-drive path");
    return canonical;
}

[[nodiscard]] std::wstring canonicalExistingDirectory(
    const std::wstring& path)
{
    UniqueHandle directory{::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(directory.valid(), "the fixture directory could not be opened");

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    require(
        ::GetFileInformationByHandleEx(
            directory.get(), FileAttributeTagInfo, &attributes,
            sizeof(attributes)) != FALSE,
        "the fixture directory attributes could not be inspected");
    require(
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U,
        "the fixture path is not a regular non-reparse directory");
    return canonicalPathForHandle(directory.get());
}

[[nodiscard]] bool missingPath(const std::wstring& path)
{
    ::SetLastError(ERROR_SUCCESS);
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const DWORD error = ::GetLastError();
    require(
        error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
        "the fixture path could not be inspected for absence");
    return true;
}

[[nodiscard]] bool removeExactOwnedDirectory(
    const std::wstring& expected) noexcept;

[[nodiscard]] std::wstring reserveUniqueAbsentDataRoot()
{
    std::array<wchar_t, MaximumPathCharacters + 1U> temporary{};
    const DWORD length = ::GetTempPathW(
        static_cast<DWORD>(temporary.size()), temporary.data());
    require(
        length > 0U && length < temporary.size(),
        "GetTempPathW could not resolve a bounded fixture parent");

    std::wstring parent{temporary.data(), static_cast<std::size_t>(length)};
    require(
        parent.size() >= 3U && parent[1U] == L':' && parent[2U] == L'\\',
        "the fixture temporary parent is not on a local DOS drive");
    if (parent.back() != L'\\') {
        parent.push_back(L'\\');
    }

    const ULONGLONG nonce = ::GetTickCount64();
    for (std::uint32_t attempt = 0U; attempt < 32U; ++attempt) {
        std::wstring candidate{parent};
        candidate += L"ForgeConductor.Manager.ProcessEnvironmentReal.";
        candidate += std::to_wstring(::GetCurrentProcessId());
        candidate.push_back(L'.');
        candidate += std::to_wstring(nonce);
        candidate.push_back(L'.');
        candidate += std::to_wstring(attempt);
        require(
            candidate.size() <= MaximumPathCharacters,
            "the unique fixture data root exceeds the path bound");

        if (::CreateDirectoryW(candidate.c_str(), nullptr) == FALSE) {
            if (::GetLastError() == ERROR_ALREADY_EXISTS) {
                continue;
            }
            throw std::runtime_error{
                "CreateDirectoryW could not reserve a unique fixture root"};
        }

        std::wstring canonical;
        try {
            canonical = canonicalExistingDirectory(candidate);
        } catch (...) {
            static_cast<void>(removeExactOwnedDirectory(candidate));
            throw;
        }
        require(
            removeExactOwnedDirectory(canonical),
            "the exact fixture root reservation could not be released safely");
        require(
            missingPath(canonical),
            "the reserved fixture data root remained after release");
        return canonical;
    }
    throw std::runtime_error{
        "a unique Manager process environment root could not be reserved"};
}

[[nodiscard]] bool directoryIsEmpty(const std::wstring& directory) noexcept
{
    try {
        std::wstring pattern{directory};
        pattern += L"\\*";
        WIN32_FIND_DATAW item{};
        UniqueFindHandle search{::FindFirstFileW(pattern.c_str(), &item)};
        if (!search.valid()) {
            return ::GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        do {
            const std::wstring_view leaf{item.cFileName};
            if (leaf != L"." && leaf != L"..") {
                return false;
            }
        } while (::FindNextFileW(search.get(), &item) != FALSE);
        return ::GetLastError() == ERROR_NO_MORE_FILES;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool removeExactOwnedDirectory(
    const std::wstring& expected) noexcept
{
    try {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD pathAttributes = ::GetFileAttributesW(expected.c_str());
        if (pathAttributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = ::GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((pathAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (pathAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return false;
        }

        {
            // Denying delete sharing keeps the validated directory identity
            // stable until the handle-relative disposition is committed.
            UniqueHandle directory{::CreateFileW(
                expected.c_str(), FILE_READ_ATTRIBUTES | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr)};
            if (!directory.valid()) {
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (::GetFileInformationByHandleEx(
                    directory.get(), FileAttributeTagInfo, &attributes,
                    sizeof(attributes)) == FALSE ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
                    0U ||
                !equalWindowsPath(
                    canonicalPathForHandle(directory.get()), expected)) {
                return false;
            }
            if (!directoryIsEmpty(expected)) {
                return false;
            }
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            if (::SetFileInformationByHandle(
                    directory.get(), FileDispositionInfo, &disposition,
                    sizeof(disposition)) == FALSE) {
                return false;
            }
        }
        ::SetLastError(ERROR_SUCCESS);
        if (::GetFileAttributesW(expected.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        const DWORD removalStatus = ::GetLastError();
        return removalStatus == ERROR_FILE_NOT_FOUND ||
            removalStatus == ERROR_PATH_NOT_FOUND;
    } catch (...) {
        return false;
    }
}

class TemporaryDataRoot final {
public:
    TemporaryDataRoot()
    {
        directories_[0] = reserveUniqueAbsentDataRoot();
        directories_[1] = directories_[0] + L"\\config";
        directories_[2] = directories_[0] + L"\\logs";
        directories_[3] = directories_[0] + L"\\exports";
        directories_[4] = directories_[0] + L"\\projects";
    }

    ~TemporaryDataRoot() noexcept
    {
        static_cast<void>(cleanup());
    }

    TemporaryDataRoot(const TemporaryDataRoot&) = delete;
    TemporaryDataRoot& operator=(const TemporaryDataRoot&) = delete;
    TemporaryDataRoot(TemporaryDataRoot&&) = delete;
    TemporaryDataRoot& operator=(TemporaryDataRoot&&) = delete;

    [[nodiscard]] const std::wstring& root() const noexcept
    {
        return directories_[0];
    }

    [[nodiscard]] const std::array<std::wstring, 5U>& directories() const
        noexcept
    {
        return directories_;
    }

    [[nodiscard]] bool missing() const
    {
        return missingPath(root());
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        if (cleaned_) {
            return true;
        }
        for (auto iterator = directories_.rbegin();
             iterator != directories_.rend(); ++iterator) {
            if (!removeExactOwnedDirectory(*iterator)) {
                return false;
            }
        }
        cleaned_ = true;
        return true;
    }

private:
    std::array<std::wstring, 5U> directories_{};
    bool cleaned_{};
};

class OperationSequence final {
public:
    [[nodiscard]] Domain::OperationContext next()
    {
        std::string operation{"60000000-0000-4000-8000-000000000000"};
        auto remaining = ++counter_;
        constexpr char Digits[] = "0123456789abcdef";
        for (std::size_t index = 0U; index < 8U; ++index) {
            operation[operation.size() - 1U - index] =
                Digits[remaining & 0xFU];
            remaining >>= 4U;
        }
        return Domain::OperationContext{
            take(Domain::OperationId::parse(operation)),
            std::chrono::steady_clock::now() + OperationTimeout,
            std::stop_token{},
            take(Domain::CorrelationId::parse(
                "manager-process-environment-real"))};
    }

private:
    std::uint32_t counter_{};
};

[[nodiscard]] std::wstring currentModuleCanonicalPath()
{
    std::array<wchar_t, MaximumPathCharacters> image{};
    const DWORD length = ::GetModuleFileNameW(
        nullptr, image.data(), static_cast<DWORD>(image.size()));
    require(
        length > 0U && length < image.size(),
        "GetModuleFileNameW could not resolve the fixture image");
    UniqueHandle file{::CreateFileW(
        image.data(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    require(file.valid(), "the current fixture image could not be opened");
    return canonicalPathForHandle(file.get());
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

[[nodiscard]] bool hasFileIdentifier(
    const Composition::ManagerProcessExecutableIdentity& identity) noexcept
{
    return std::ranges::any_of(
        identity.fileIdentifier,
        [](const std::byte value) noexcept { return value != std::byte{}; });
}

[[nodiscard]] std::vector<std::wstring> directoryLeaves(
    const std::wstring& directory)
{
    std::wstring pattern{directory};
    pattern += L"\\*";
    WIN32_FIND_DATAW item{};
    UniqueFindHandle search{::FindFirstFileW(pattern.c_str(), &item)};
    require(search.valid(), "the prepared data root could not be enumerated");

    std::vector<std::wstring> leaves;
    do {
        const std::wstring_view leaf{item.cFileName};
        if (leaf == L"." || leaf == L"..") {
            continue;
        }
        require(
            (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U,
            "the prepared data root contains a non-directory or reparse entry");
        leaves.emplace_back(leaf);
    } while (::FindNextFileW(search.get(), &item) != FALSE);
    require(
        ::GetLastError() == ERROR_NO_MORE_FILES,
        "the prepared data-root enumeration ended unexpectedly");
    std::ranges::sort(leaves);
    return leaves;
}

void inspectPrepareAndReleaseTheRealManagerEnvironment()
{
    TemporaryDataRoot temporaryRoot;
    require(
        temporaryRoot.missing(),
        "the unique data root existed before read-only inspection");

    Composition::WindowsManagerProcessEnvironmentPlatformProbe platform;
    Composition::ManagerProcessEnvironment environment{
        Composition::ManagerProcessEnvironmentOptions{
            pathText(temporaryRoot.root()), false},
        platform};
    OperationSequence operations;

    const auto inspected = take(environment.inspect(operations.next()));
    require(
        temporaryRoot.missing(),
        "read-only environment inspection created the explicit data root");
    const auto reinspected = take(environment.inspect(operations.next()));
    require(
        temporaryRoot.missing(),
        "repeated read-only inspection mutated the explicit data root");
    require(
        reinspected == inspected,
        "repeated native inspection returned unstable process evidence");

    const auto& expectedDirectories = temporaryRoot.directories();
    const std::array<const Domain::PathText*, 5U> snapshotDirectories{
        &inspected.dataRoot(), &inspected.configurationRoot(),
        &inspected.diagnosticsRoot(), &inspected.exportRoot(),
        &inspected.projectsRoot()};
    for (std::size_t index = 0U; index < snapshotDirectories.size(); ++index) {
        require(
            equalWindowsPath(
                utf8ToWide(snapshotDirectories[index]->value()),
                expectedDirectories[index]),
            "the inspected environment changed an exact fixture directory");
    }

    const std::wstring managerPath =
        utf8ToWide(inspected.managerExecutable().value());
    const std::wstring cliPath = utf8ToWide(inspected.cliExecutable().value());
    require(
        equalWindowsPath(managerPath, currentModuleCanonicalPath()),
        "the native probe did not identify the current Manager image");
    require(
        equalWindowsPath(leafName(managerPath), L"ForgeConductor.Manager.exe"),
        "the fixture image does not have the required Manager leaf name");
    require(
        equalWindowsPath(leafName(cliPath), L"forge-conductor.exe"),
        "the native probe did not identify the exact sibling CLI leaf");
    require(
        equalWindowsPath(parentPath(managerPath), parentPath(cliPath)),
        "the Manager and CLI images are not exact filesystem siblings");
    require(
        inspected.managerExecutableIdentity() ==
                reinspected.managerExecutableIdentity() &&
            inspected.cliExecutableIdentity() ==
                reinspected.cliExecutableIdentity(),
        "the native executable identities changed across inspections");
    require(
        hasFileIdentifier(inspected.managerExecutableIdentity()) &&
            hasFileIdentifier(inspected.cliExecutableIdentity()),
        "a native executable identity did not retain a stable file identifier");
    require(
        inspected.managerExecutableIdentity().volumeSerialNumber !=
                inspected.cliExecutableIdentity().volumeSerialNumber ||
            inspected.managerExecutableIdentity().fileIdentifier !=
                inspected.cliExecutableIdentity().fileIdentifier,
        "the distinct Manager and CLI images reported the same native file identity");

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    require(
        ::GlobalMemoryStatusEx(&memory) != FALSE,
        "GlobalMemoryStatusEx could not provide fixture evidence");
    require(
        inspected.physicalMemoryBytes() ==
            static_cast<std::uint64_t>(memory.ullTotalPhys),
        "the process environment did not retain real physical-memory evidence");
    const auto expectedProfile =
        Domain::selectResourceProfile(inspected.physicalMemoryBytes());
    require(
        inspected.resourceProfile() == expectedProfile,
        "the real physical-memory evidence selected the wrong profile");
    require(
        inspected.resourceBudgets() == Domain::budgetsForProfile(expectedProfile),
        "the real resource profile selected the wrong bounded budgets");

    const auto user = take(Infrastructure::WindowsCurrentUserIdentity::load());
    std::string leaseSuffix{
        "p16-env-real-" + std::to_string(::GetCurrentProcessId()) + '-' +
        std::to_string(::GetTickCount64())};
    require(
        leaseSuffix.size() <=
            Infrastructure::WindowsManagerInstanceLease::
                MaximumPurposeSuffixCharacters,
        "the isolated real-process lease suffix exceeds its production bound");
    const Infrastructure::WindowsManagerInstanceLeaseOptions leaseOptions{
        std::move(leaseSuffix)};
    {
        auto lease = take(Infrastructure::WindowsManagerInstanceLease::acquire(
            user, leaseOptions));
        require(lease.owns(), "the real per-user Manager lease was not acquired");

        auto prepared = take(environment.prepareAfterLease(
            inspected, std::move(lease), operations.next()));
        require(
            prepared.lease().owns(),
            "the prepared process environment lost its Manager lease");
        require(
            prepared.snapshot() == inspected,
            "preparation changed the inspected immutable environment");

        for (std::size_t index = 0U; index < expectedDirectories.size();
             ++index) {
            const std::wstring canonical =
                canonicalExistingDirectory(expectedDirectories[index]);
            require(
                equalWindowsPath(canonical, expectedDirectories[index]),
                "a prepared fixture directory changed canonical identity");
        }
        std::vector<std::wstring> expectedLeaves;
        expectedLeaves.reserve(ExpectedChildLeaves.size());
        for (const std::wstring_view leaf : ExpectedChildLeaves) {
            expectedLeaves.emplace_back(leaf);
        }
        require(
            directoryLeaves(expectedDirectories[0]) == expectedLeaves,
            "preparation created a directory set other than the exact five required roots");
        for (std::size_t index = 1U; index < expectedDirectories.size();
             ++index) {
            require(
                directoryIsEmpty(expectedDirectories[index]),
                "a prepared Manager process child directory was not empty");
        }
    }

    {
        auto reacquired = take(
            Infrastructure::WindowsManagerInstanceLease::acquire(
                user, leaseOptions));
        require(
            reacquired.owns(),
            "destroying the prepared environment did not release the real lease");
    }

    require(
        temporaryRoot.cleanup(),
        "the exact test-owned directories could not be removed in reverse order");
    require(
        temporaryRoot.missing(),
        "the explicit fixture data root remained after safe cleanup");
}

} // namespace

int main()
{
    try {
        inspectPrepareAndReleaseTheRealManagerEnvironment();
        std::cout
            << "Manager process environment real-process tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Manager process environment real-process tests failed: "
            << error.what() << '\n';
        return 1;
    }
}
