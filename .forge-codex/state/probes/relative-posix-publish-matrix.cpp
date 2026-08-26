#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr ULONG FileRenameInformationNative = 10U;
constexpr ULONG FileRenameInformationExNative = 65U;

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

HANDLE openRelative(HANDLE parent, wchar_t* component, ACCESS_MASK access, ULONG sharing,
                    ULONG disposition)
{
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(std::wcslen(component) * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = component;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, parent, nullptr);
    IO_STATUS_BLOCK ioStatus{};
    HANDLE result{};
    const NTSTATUS status = ::NtCreateFile(
        &result, access | SYNCHRONIZE, &attributes, &ioStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
        sharing, disposition,
        FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0U);
    if (status < 0) {
        ::SetLastError(::RtlNtStatusToDosError(status));
        return INVALID_HANDLE_VALUE;
    }
    return result;
}

void closeIfValid(HANDLE handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
    }
}

struct Case final {
    std::string_view name;
    DWORD parentSharing;
    bool targetExists;
    bool pinTarget;
    bool pinSharesDelete;
    bool useParentRoot;
    bool useExtendedClass;
    bool expectedSuccess;
};

int runCase(const Case& testCase)
{
    wchar_t temporaryRoot[MAX_PATH]{};
    wchar_t directoryName[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, temporaryRoot) == 0U ||
        ::GetTempFileNameW(temporaryRoot, L"fcp", 0U, directoryName) == 0U ||
        ::DeleteFileW(directoryName) == FALSE ||
        ::CreateDirectoryW(directoryName, nullptr) == FALSE) {
        std::cout << testCase.name << " fixture=" << ::GetLastError() << '\n';
        return 1;
    }
    const std::wstring targetPath = std::wstring{directoryName} + L"\\target.bin";
    const std::wstring stagedPath = std::wstring{directoryName} + L"\\staged.bin";
    if (testCase.targetExists) {
        HANDLE initial = ::CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        constexpr char OldBytes[] = "old";
        DWORD transferred{};
        if (initial == INVALID_HANDLE_VALUE ||
            ::WriteFile(initial, OldBytes, 3U, &transferred, nullptr) == FALSE ||
            transferred != 3U) {
            closeIfValid(initial);
            std::cout << testCase.name << " initial=" << ::GetLastError() << '\n';
            return 1;
        }
        ::CloseHandle(initial);
    }

    HANDLE parent = ::CreateFileW(
        directoryName, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                           FILE_ADD_FILE | FILE_DELETE_CHILD,
        testCase.parentSharing, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    wchar_t targetName[] = L"target.bin";
    wchar_t stagedName[] = L"staged.bin";
    HANDLE oldTarget = INVALID_HANDLE_VALUE;
    if (testCase.targetExists && testCase.pinTarget) {
        oldTarget = openRelative(parent, targetName, GENERIC_READ | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ |
                                     (testCase.pinSharesDelete ? FILE_SHARE_DELETE : 0U),
                                 FILE_OPEN);
    }
    HANDLE staged = openRelative(parent, stagedName,
                                 GENERIC_WRITE | GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                                 0U, FILE_CREATE);
    constexpr char NewBytes[] = "new";
    DWORD transferred{};
    if (parent == INVALID_HANDLE_VALUE || staged == INVALID_HANDLE_VALUE ||
        (testCase.targetExists && testCase.pinTarget && oldTarget == INVALID_HANDLE_VALUE) ||
        ::WriteFile(staged, NewBytes, 3U, &transferred, nullptr) == FALSE || transferred != 3U ||
        ::FlushFileBuffers(staged) == FALSE) {
        std::cout << testCase.name << " open_or_write=" << ::GetLastError() << '\n';
        closeIfValid(staged);
        closeIfValid(oldTarget);
        closeIfValid(parent);
        return 1;
    }

    const DWORD nameBytes = static_cast<DWORD>(std::wcslen(targetName) * sizeof(wchar_t));
    const std::size_t informationBytes = offsetof(FILE_RENAME_INFO, FileName) + nameBytes;
    std::vector<std::uint64_t> storage(
        (informationBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
    auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(information, 0, storage.size() * sizeof(std::uint64_t));
    information->Flags = testCase.targetExists
                             ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                                   FILE_RENAME_FLAG_POSIX_SEMANTICS
                             : 0U;
    information->RootDirectory = testCase.useParentRoot ? parent : nullptr;
    information->FileNameLength = nameBytes;
    std::memcpy(information->FileName, targetName, nameBytes);

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto ntSet = reinterpret_cast<NtSetInformationFileFunction>(
        ::GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto rtlError = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSet(
        staged, &ioStatus, information, static_cast<ULONG>(informationBytes),
        testCase.useExtendedClass ? FileRenameInformationExNative : FileRenameInformationNative);
    const DWORD error = status >= 0 ? ERROR_SUCCESS : rtlError(status);

    char oldObserved[4]{};
    DWORD oldObservedBytes{};
    if (oldTarget != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER beginning{};
        ::SetFilePointerEx(oldTarget, beginning, nullptr, FILE_BEGIN);
        ::ReadFile(oldTarget, oldObserved, 3U, &oldObservedBytes, nullptr);
    }
    closeIfValid(staged);
    staged = INVALID_HANDLE_VALUE;
    HANDLE current = ::CreateFileW(targetPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    char observed[4]{};
    DWORD observedBytes{};
    if (current != INVALID_HANDLE_VALUE) {
        ::ReadFile(current, observed, 3U, &observedBytes, nullptr);
    }
    std::cout << testCase.name << " status=0x" << std::hex
              << static_cast<std::uint32_t>(status) << std::dec << " error=" << error
              << " old-handle=" << std::string{oldObserved, oldObservedBytes}
              << " current=" << std::string{observed, observedBytes} << '\n';

    const bool succeeded = status >= 0;
    const bool currentIsExpected =
        succeeded ? std::string{observed, observedBytes} == "new"
                  : (testCase.targetExists
                         ? std::string{observed, observedBytes} == "old"
                         : current == INVALID_HANDLE_VALUE);
    const bool pinnedOldIsExpected =
        !testCase.pinTarget || std::string{oldObserved, oldObservedBytes} == "old";

    closeIfValid(current);
    closeIfValid(staged);
    closeIfValid(oldTarget);
    closeIfValid(parent);
    ::DeleteFileW(targetPath.c_str());
    ::DeleteFileW(stagedPath.c_str());
    ::RemoveDirectoryW(directoryName);
    return succeeded == testCase.expectedSuccess && currentIsExpected && pinnedOldIsExpected
               ? 0
               : 1;
}

} // namespace

int wmain()
{
    constexpr DWORD ShareRead = FILE_SHARE_READ;
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const Case cases[] = {
        {"ex-root/read/existing/pinned-read", ShareRead, true, true, false, true, true, false},
        {"ex-root/all/existing/pinned-read", ShareAll, true, true, false, true, true, false},
        {"ex-root/read/existing/pinned-delete", ShareRead, true, true, true, true, true, false},
        {"ex-root/all/existing/pinned-delete", ShareAll, true, true, true, true, true, true},
        {"ex-root/read/existing/unpinned", ShareRead, true, false, false, true, true, false},
        {"ex-root/all/existing/unpinned", ShareAll, true, false, false, true, true, true},
        {"ex-root/read/absent", ShareRead, false, false, false, true, true, false},
        {"ex-root/all/absent", ShareAll, false, false, false, true, true, true},
        {"ex-null/read/existing/pinned-read", ShareRead, true, true, false, false, true, false},
        {"ex-null/all/existing/pinned-read", ShareAll, true, true, false, false, true, false},
        {"ex-null/read/existing/pinned-delete", ShareRead, true, true, true, false, true, true},
        {"ex-null/all/existing/pinned-delete", ShareAll, true, true, true, false, true, true},
        {"ex-null/read/existing/unpinned", ShareRead, true, false, false, false, true, true},
        {"ex-null/all/existing/unpinned", ShareAll, true, false, false, false, true, true},
        {"ex-null/read/absent", ShareRead, false, false, false, false, true, true},
        {"ex-null/all/absent", ShareAll, false, false, false, false, true, true},
        {"legacy-root/all/absent", ShareAll, false, false, false, true, false, true},
        {"legacy-null/all/absent", ShareAll, false, false, false, false, false, true},
    };
    int failures{};
    for (const auto& testCase : cases) {
        failures += runCase(testCase);
    }
    return failures == 0 ? 0 : 1;
}
