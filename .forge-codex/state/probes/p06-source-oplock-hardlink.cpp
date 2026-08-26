#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr ULONG FileLinkInformationNative = 11U;

struct NativeIoStatusBlock final {
    union {
        LONG status;
        void* pointer;
    } result{};
    ULONG_PTR information{};
};

using NtSetInformationFileFunction =
    LONG(NTAPI*)(HANDLE, NativeIoStatusBlock*, void*, ULONG, ULONG);

class Handle final {
public:
    Handle() noexcept = default;
    explicit Handle(const HANDLE value) noexcept : value_{value} {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(const HANDLE replacement = nullptr) noexcept
    {
        if (valid()) {
            ::CloseHandle(value_);
        }
        value_ = replacement;
    }

private:
    HANDLE value_{};
};

struct Fixture final {
    std::wstring directory;

    ~Fixture()
    {
        if (!directory.empty()) {
            ::RemoveDirectoryW(directory.c_str());
        }
    }
};

[[nodiscard]] bool makeFixture(Fixture& fixture)
{
    std::array<wchar_t, MAX_PATH> temporary{};
    std::array<wchar_t, MAX_PATH> placeholder{};
    if (::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) == 0U ||
        ::GetTempFileNameW(temporary.data(), L"fop", 0U, placeholder.data()) == 0U ||
        ::DeleteFileW(placeholder.data()) == FALSE ||
        ::CreateDirectoryW(placeholder.data(), nullptr) == FALSE) {
        return false;
    }
    fixture.directory = placeholder.data();
    return true;
}

[[nodiscard]] std::wstring child(const Fixture& fixture, const std::wstring_view name)
{
    std::wstring path = fixture.directory;
    path.push_back(L'\\');
    path.append(name);
    return path;
}

struct HardLinkContext final {
    const wchar_t* alias{};
    const wchar_t* source{};
    BOOL succeeded{};
    DWORD error{};
};

DWORD WINAPI createHardLink(void* raw) noexcept
{
    auto& context = *static_cast<HardLinkContext*>(raw);
    context.succeeded = ::CreateHardLinkW(context.alias, context.source, nullptr);
    context.error = context.succeeded != FALSE ? ERROR_SUCCESS : ::GetLastError();
    return 0U;
}

[[nodiscard]] bool sourceOplockDoesNotSerializeHardLink(const Fixture& fixture)
{
    const auto source = child(fixture, L"oplocked-stage.tmp");
    const auto alias = child(fixture, L"oplocked-alias.tmp");
    constexpr DWORD DesiredAccess =
        FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_READ_EA | DELETE;
    constexpr DWORD Flags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
                            FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REQUIRING_OPLOCK |
                            FILE_FLAG_OPEN_REPARSE_POINT;
    Handle stage{::CreateFileW(source.c_str(), DesiredAccess, 0U, nullptr, CREATE_NEW, Flags,
                               nullptr)};
    if (!stage.valid()) {
        std::cout << "OPLOCK create_error=" << ::GetLastError() << '\n';
        return false;
    }

    Handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) {
        std::cout << "OPLOCK event_error=" << ::GetLastError() << '\n';
        return false;
    }

    REQUEST_OPLOCK_INPUT_BUFFER input{};
    input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    input.StructureLength = sizeof(input);
    input.RequestedOplockLevel =
        OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_WRITE | OPLOCK_LEVEL_CACHE_HANDLE;
    input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    REQUEST_OPLOCK_OUTPUT_BUFFER output{};
    output.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    output.StructureLength = sizeof(output);
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned{};
    const BOOL immediate = ::DeviceIoControl(
        stage.get(), FSCTL_REQUEST_OPLOCK, &input, sizeof(input), &output, sizeof(output), &returned,
        &overlapped);
    const DWORD requestError = immediate != FALSE ? ERROR_SUCCESS : ::GetLastError();
    if (immediate != FALSE || requestError != ERROR_IO_PENDING) {
        std::cout << "OPLOCK request_immediate=" << (immediate != FALSE)
                  << " request_error=" << requestError << '\n';
        return false;
    }

    HardLinkContext context{alias.c_str(), source.c_str()};
    Handle thread{::CreateThread(nullptr, 0U, createHardLink, &context, 0U, nullptr)};
    if (!thread.valid()) {
        std::cout << "OPLOCK thread_error=" << ::GetLastError() << '\n';
        return false;
    }
    const DWORD linkWait = ::WaitForSingleObject(thread.get(), 1'000U);
    const DWORD oplockWait = ::WaitForSingleObject(event.get(), 0U);

    stage.reset();
    static_cast<void>(::WaitForSingleObject(thread.get(), 3'000U));
    std::cout << "OPLOCK request=ERROR_IO_PENDING"
              << " link_wait=" << linkWait << " link_succeeded=" << (context.succeeded != FALSE)
              << " link_error=" << context.error << " oplock_wait=" << oplockWait << '\n';

    static_cast<void>(::DeleteFileW(alias.c_str()));
    static_cast<void>(::DeleteFileW(source.c_str()));
    return linkWait == WAIT_OBJECT_0 && context.succeeded != FALSE &&
           oplockWait == WAIT_TIMEOUT;
}

[[nodiscard]] Handle createStage(const std::wstring& path)
{
    return Handle{::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr)};
}

[[nodiscard]] bool posixUnlink(const std::wstring& path)
{
    Handle deletion{::CreateFileW(path.c_str(), DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!deletion.valid()) {
        return false;
    }
    FILE_DISPOSITION_INFO_EX disposition{};
    disposition.Flags =
        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (::SetFileInformationByHandle(deletion.get(), FileDispositionInfoEx, &disposition,
                                     sizeof(disposition)) == FALSE) {
        return false;
    }
    deletion.reset();
    return ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
           ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

[[nodiscard]] bool unlinkedHandleCannotBeRenamed(const Fixture& fixture)
{
    const auto source = child(fixture, L"unlink-rename-stage.tmp");
    const auto destination = child(fixture, L"unlink-rename-destination.bin");
    auto stage = createStage(source);
    if (!stage.valid() || !posixUnlink(source)) {
        std::cout << "POSIX_RENAME setup_error=" << ::GetLastError() << '\n';
        return false;
    }

    FILE_DISPOSITION_INFO_EX clearDisposition{};
    const BOOL cleared = ::SetFileInformationByHandle(stage.get(), FileDispositionInfoEx,
                                                       &clearDisposition,
                                                       sizeof(clearDisposition));
    const DWORD clearError = cleared != FALSE ? ERROR_SUCCESS : ::GetLastError();

    const std::size_t nameBytes = destination.size() * sizeof(wchar_t);
    const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                       sizeof(std::uint64_t));
    auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(information, 0, informationBytes);
    information->Flags = 0U;
    information->RootDirectory = nullptr;
    information->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(information->FileName, destination.data(), nameBytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        stage.get(), FileRenameInfoEx, information, static_cast<DWORD>(informationBytes));
    const DWORD renameError = renamed != FALSE ? ERROR_SUCCESS : ::GetLastError();
    std::cout << "POSIX_RENAME name_removed=1 clear_succeeded=" << (cleared != FALSE)
              << " clear_error=" << clearError << " rename_succeeded=" << (renamed != FALSE)
              << " rename_error=" << renameError << '\n';

    stage.reset();
    static_cast<void>(::DeleteFileW(destination.c_str()));
    return cleared == FALSE && clearError == ERROR_ACCESS_DENIED && renamed == FALSE &&
           renameError == ERROR_ACCESS_DENIED;
}

[[nodiscard]] bool unlinkedHandleCannotBeRelinked(const Fixture& fixture)
{
    const auto source = child(fixture, L"unlink-link-stage.tmp");
    const auto destinationName = std::wstring{L"unlink-link-destination.bin"};
    const auto destination = child(fixture, destinationName);
    auto stage = createStage(source);
    if (!stage.valid() || !posixUnlink(source)) {
        std::cout << "POSIX_LINK setup_error=" << ::GetLastError() << '\n';
        return false;
    }
    Handle directory{::CreateFileW(
        fixture.directory.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!directory.valid()) {
        std::cout << "POSIX_LINK directory_error=" << ::GetLastError() << '\n';
        return false;
    }

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto ntSetInformationFile =
        ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtSetInformationFileFunction>(
                  ::GetProcAddress(ntdll, "NtSetInformationFile"));
    if (ntSetInformationFile == nullptr) {
        std::cout << "POSIX_LINK native_error=" << ERROR_PROC_NOT_FOUND << '\n';
        return false;
    }

    const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
    const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                       sizeof(std::uint64_t));
    auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(information, 0, informationBytes);
    information->ReplaceIfExists = FALSE;
    information->RootDirectory = directory.get();
    information->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(information->FileName, destinationName.data(), nameBytes);
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSetInformationFile(stage.get(), &ioStatus, information,
                                             static_cast<ULONG>(informationBytes),
                                             FileLinkInformationNative);
    std::cout << "POSIX_LINK name_removed=1 link_status=0x" << std::hex << std::uppercase
              << static_cast<std::uint32_t>(status) << std::dec
              << " destination_exists="
              << (::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) << '\n';

    stage.reset();
    static_cast<void>(::DeleteFileW(destination.c_str()));
    return static_cast<std::uint32_t>(status) == 0xC0000022U;
}

} // namespace

int wmain()
{
    Fixture fixture;
    if (!makeFixture(fixture)) {
        std::cout << "FIXTURE error=" << ::GetLastError() << '\n';
        return 1;
    }
    const bool oplock = sourceOplockDoesNotSerializeHardLink(fixture);
    const bool rename = unlinkedHandleCannotBeRenamed(fixture);
    const bool link = unlinkedHandleCannotBeRelinked(fixture);
    std::cout << "RESULT source_oplock_bypassed=" << oplock
              << " unlinked_rename_rejected=" << rename
              << " unlinked_relink_rejected=" << link << '\n';
    return oplock && rename && link ? 0 : 1;
}
