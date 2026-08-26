#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

#include <iostream>
#include <string>

int wmain()
{
    wchar_t temporaryRoot[MAX_PATH]{};
    wchar_t temporaryName[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, temporaryRoot) == 0U ||
        ::GetTempFileNameW(temporaryRoot, L"fca", 0U, temporaryName) == 0U ||
        ::DeleteFileW(temporaryName) == FALSE ||
        ::CreateDirectoryW(temporaryName, nullptr) == FALSE) {
        std::wcerr << L"fixture_error=" << ::GetLastError() << L'\n';
        return 2;
    }

    HANDLE parent = ::CreateFileW(
        temporaryName, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent == INVALID_HANDLE_VALUE) {
        std::wcerr << L"parent_error=" << ::GetLastError() << L'\n';
        ::RemoveDirectoryW(temporaryName);
        return 3;
    }

    wchar_t childName[] = L"child";
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(5U * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = childName;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, parent, nullptr);
    IO_STATUS_BLOCK ioStatus{};
    HANDLE child{};
    const NTSTATUS status = ::NtCreateFile(
        &child, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
        &attributes, &ioStatus, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_CREATE,
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0U);

    std::wcout << L"ntstatus=" << static_cast<unsigned long>(status)
               << L" win32=" << ::RtlNtStatusToDosError(status)
               << L" information=" << ioStatus.Information << L'\n';
    if (child != nullptr && child != INVALID_HANDLE_VALUE) {
        ::CloseHandle(child);
    }
    ::CloseHandle(parent);
    const std::wstring childPath = std::wstring{temporaryName} + L"\\child";
    ::RemoveDirectoryW(childPath.c_str());
    ::RemoveDirectoryW(temporaryName);
    return status < 0 ? 1 : 0;
}
