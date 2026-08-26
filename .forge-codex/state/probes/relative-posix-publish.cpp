#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

} // namespace

int wmain()
{
    wchar_t temporaryRoot[MAX_PATH]{};
    wchar_t directoryName[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, temporaryRoot) == 0U ||
        ::GetTempFileNameW(temporaryRoot, L"fcp", 0U, directoryName) == 0U ||
        ::DeleteFileW(directoryName) == FALSE ||
        ::CreateDirectoryW(directoryName, nullptr) == FALSE) {
        std::wcerr << L"fixture_error=" << ::GetLastError() << L'\n';
        return 2;
    }
    const std::wstring targetPath = std::wstring{directoryName} + L"\\target.bin";
    const std::wstring stagedPath = std::wstring{directoryName} + L"\\staged.bin";
    HANDLE initial = ::CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    constexpr char OldBytes[] = "old";
    DWORD transferred{};
    if (initial == INVALID_HANDLE_VALUE ||
        ::WriteFile(initial, OldBytes, 3U, &transferred, nullptr) == FALSE || transferred != 3U) {
        std::wcerr << L"initial_error=" << ::GetLastError() << L'\n';
        closeIfValid(initial);
        return 3;
    }
    ::CloseHandle(initial);

    HANDLE parent = ::CreateFileW(
        directoryName, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                           FILE_ADD_FILE | FILE_DELETE_CHILD,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    wchar_t targetName[] = L"target.bin";
    wchar_t stagedName[] = L"staged.bin";
    HANDLE oldTarget = openRelative(parent, targetName, GENERIC_READ | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ, FILE_OPEN);
    HANDLE staged = openRelative(parent, stagedName,
                                 GENERIC_WRITE | GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                                 0U, FILE_CREATE);
    constexpr char NewBytes[] = "new";
    transferred = 0U;
    if (parent == INVALID_HANDLE_VALUE || oldTarget == INVALID_HANDLE_VALUE ||
        staged == INVALID_HANDLE_VALUE ||
        ::WriteFile(staged, NewBytes, 3U, &transferred, nullptr) == FALSE || transferred != 3U ||
        ::FlushFileBuffers(staged) == FALSE) {
        std::wcerr << L"open_or_write_error=" << ::GetLastError() << L'\n';
        closeIfValid(staged);
        closeIfValid(oldTarget);
        closeIfValid(parent);
        return 4;
    }

    DWORD filesystemFlags{};
    if (::GetVolumeInformationByHandleW(parent, nullptr, 0U, nullptr, nullptr, &filesystemFlags,
                                        nullptr, 0U) == FALSE) {
        std::wcerr << L"volume_error=" << ::GetLastError() << L'\n';
        return 5;
    }

    const std::wstring renameDestination = targetName;
    const DWORD nameBytes = static_cast<DWORD>(renameDestination.size() * sizeof(wchar_t));
    std::vector<std::byte> renameStorage(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameStorage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = parent;
    rename->FileNameLength = nameBytes;
    std::memcpy(rename->FileName, renameDestination.data(), nameBytes);
    const BOOL published = ::SetFileInformationByHandle(
        staged, FileRenameInfo, rename, static_cast<DWORD>(renameStorage.size()));
    const DWORD publishError = published != FALSE ? ERROR_SUCCESS : ::GetLastError();

    LARGE_INTEGER zero{};
    ::SetFilePointerEx(oldTarget, zero, nullptr, FILE_BEGIN);
    char oldObserved[4]{};
    DWORD oldRead{};
    const BOOL oldReadResult = ::ReadFile(oldTarget, oldObserved, 3U, &oldRead, nullptr);
    HANDLE current = ::CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    char newObserved[4]{};
    DWORD newRead{};
    const BOOL newReadResult = current != INVALID_HANDLE_VALUE
                                   ? ::ReadFile(current, newObserved, 3U, &newRead, nullptr)
                                   : FALSE;
    std::cout << "feature="
              << ((filesystemFlags & FILE_SUPPORTS_POSIX_UNLINK_RENAME) != 0U)
              << " published=" << (published != FALSE) << " error=" << publishError
              << " old=" << std::string{oldObserved, oldRead}
              << " current=" << std::string{newObserved, newRead} << '\n';

    closeIfValid(current);
    closeIfValid(staged);
    closeIfValid(oldTarget);
    closeIfValid(parent);
    ::DeleteFileW(targetPath.c_str());
    ::DeleteFileW(stagedPath.c_str());
    ::RemoveDirectoryW(directoryName);
    return published != FALSE && oldReadResult != FALSE && newReadResult != FALSE &&
                   std::string{oldObserved, oldRead} == "old" &&
                   std::string{newObserved, newRead} == "new"
               ? 0
               : 1;
}
