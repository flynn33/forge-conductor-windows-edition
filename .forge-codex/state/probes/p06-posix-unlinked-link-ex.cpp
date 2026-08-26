#define NOMINMAX
#include <Windows.h>

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

constexpr ULONG NativeFileLinkInformationEx = 72U;
constexpr ULONG FileLinkFlagPosixSemantics = 0x00000002U;

struct NativeIoStatusBlock final {
    union {
        LONG status;
        void* pointer;
    } result{};
    ULONG_PTR information{};
};

struct NativeFileLinkInformationExBuffer final {
    ULONG flags{};
    HANDLE rootDirectory{};
    ULONG fileNameLength{};
    WCHAR fileName[1]{};
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
        ::GetTempFileNameW(temporary.data(), L"flx", 0U, placeholder.data()) == 0U ||
        ::DeleteFileW(placeholder.data()) == FALSE ||
        ::CreateDirectoryW(placeholder.data(), nullptr) == FALSE) {
        return false;
    }
    fixture.directory = placeholder.data();
    return true;
}

[[nodiscard]] std::wstring child(const Fixture& fixture, const std::wstring_view name)
{
    std::wstring result = fixture.directory;
    result.push_back(L'\\');
    result.append(name);
    return result;
}

[[nodiscard]] bool runAttempt(const Fixture& fixture, const ULONG linkFlags,
                              const std::wstring_view label)
{
    const std::wstring sourceName = std::wstring{L"stage-"} + std::wstring{label} + L".tmp";
    const std::wstring destinationName = std::wstring{L"published-"} + std::wstring{label} + L".bin";
    const auto source = child(fixture, sourceName);
    const auto destination = child(fixture, destinationName);

    Handle directory{::CreateFileW(
        fixture.directory.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    constexpr DWORD DesiredAccess =
        FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_READ_EA | DELETE;
    Handle stage{::CreateFileW(source.c_str(), DesiredAccess,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
                                   FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr)};
    if (!directory.valid() || !stage.valid()) {
        std::wcout << label << L" setup_error=" << ::GetLastError() << L'\n';
        return false;
    }

    Handle deletion{::CreateFileW(source.c_str(), DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FILE_DISPOSITION_INFO_EX disposition{};
    disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    const BOOL unlinked =
        deletion.valid() != false
            ? ::SetFileInformationByHandle(deletion.get(), FileDispositionInfoEx, &disposition,
                                           sizeof(disposition))
            : FALSE;
    const DWORD unlinkError = unlinked != FALSE ? ERROR_SUCCESS : ::GetLastError();
    deletion.reset();
    const bool nameRemoved = ::GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES &&
                             ::GetLastError() == ERROR_FILE_NOT_FOUND;

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto ntSetInformationFile =
        ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtSetInformationFileFunction>(
                  ::GetProcAddress(ntdll, "NtSetInformationFile"));
    if (ntSetInformationFile == nullptr) {
        std::wcout << label << L" native_error=" << ERROR_PROC_NOT_FOUND << L'\n';
        return false;
    }

    const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
    const std::size_t informationBytes =
        offsetof(NativeFileLinkInformationExBuffer, fileName) + nameBytes;
    std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) /
                                       sizeof(std::uint64_t));
    auto* const information =
        reinterpret_cast<NativeFileLinkInformationExBuffer*>(storage.data());
    std::memset(information, 0, informationBytes);
    information->flags = linkFlags;
    information->rootDirectory = directory.get();
    information->fileNameLength = static_cast<ULONG>(nameBytes);
    std::memcpy(information->fileName, destinationName.data(), nameBytes);
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSetInformationFile(stage.get(), &ioStatus, information,
                                             static_cast<ULONG>(informationBytes),
                                             NativeFileLinkInformationEx);
    const bool destinationBeforeClose =
        ::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES;

    FILE_DISPOSITION_INFO_EX clearDisposition{};
    const BOOL cleared = ::SetFileInformationByHandle(stage.get(), FileDispositionInfoEx,
                                                       &clearDisposition, sizeof(clearDisposition));
    const DWORD clearError = cleared != FALSE ? ERROR_SUCCESS : ::GetLastError();
    stage.reset();
    const bool destinationAfterClose =
        ::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES;

    std::wcout << label << L" unlink_succeeded=" << (unlinked != FALSE)
              << L" unlink_error=" << unlinkError << L" name_removed=" << nameRemoved
              << L" link_status=0x" << std::hex << std::uppercase
              << static_cast<std::uint32_t>(status) << std::dec
              << L" destination_before_close=" << destinationBeforeClose
              << L" clear_succeeded=" << (cleared != FALSE) << L" clear_error=" << clearError
              << L" destination_after_close=" << destinationAfterClose << L'\n';
    static_cast<void>(::DeleteFileW(destination.c_str()));
    static_cast<void>(::DeleteFileW(source.c_str()));
    return unlinked != FALSE && nameRemoved && status < 0 && !destinationBeforeClose &&
           !destinationAfterClose;
}

} // namespace

int wmain()
{
    Fixture fixture;
    if (!makeFixture(fixture)) {
        std::cout << "FIXTURE error=" << ::GetLastError() << '\n';
        return 2;
    }
    const bool flagsZero = runAttempt(fixture, 0U, L"flags-zero");
    const bool posix = runAttempt(fixture, FileLinkFlagPosixSemantics, L"flags-posix");
    std::cout << "RESULT class72_flags_zero_rejected=" << flagsZero
              << " class72_posix_rejected=" << posix << '\n';
    return flagsZero && posix ? 0 : 1;
}
