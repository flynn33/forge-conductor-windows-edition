#include "RelativeFileOperations.h"

#include <winternl.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr NTSTATUS InvalidHandleStatus = static_cast<NTSTATUS>(0xC0000008UL);
constexpr NTSTATUS InvalidParameterStatus = static_cast<NTSTATUS>(0xC000000DUL);
constexpr NTSTATUS InvalidNameStatus = static_cast<NTSTATUS>(0xC0000033UL);
constexpr NTSTATUS NoMemoryStatus = static_cast<NTSTATUS>(0xC0000017UL);

[[nodiscard]] bool validOneComponentName(const std::wstring_view value) noexcept
{
    if (value.empty() || value == L"." || value == L".." || value.back() == L' ' ||
        value.back() == L'.' ||
        value.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
        return false;
    }
    const bool invalidCharacter =
        std::any_of(value.begin(), value.end(), [](const wchar_t character) noexcept {
            return character < 0x20 || character == L'\\' || character == L'/' ||
                   character == L':' || character == L'<' || character == L'>' ||
                   character == L'"' || character == L'|' || character == L'?' || character == L'*';
        });
    if (invalidCharacter) {
        return false;
    }

    std::wstring_view base = value.substr(0U, value.find(L'.'));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) {
        base.remove_suffix(1U);
    }
    const auto equals = [base](const std::wstring_view reserved) noexcept {
        return base.size() == reserved.size() &&
               ::CompareStringOrdinal(base.data(), static_cast<int>(base.size()), reserved.data(),
                                      static_cast<int>(reserved.size()), TRUE) == CSTR_EQUAL;
    };
    if (equals(L"CON") || equals(L"PRN") || equals(L"AUX") || equals(L"NUL") || equals(L"CONIN$") ||
        equals(L"CONOUT$") || equals(L"CLOCK$")) {
        return false;
    }
    if (base.size() != 4U) {
        return true;
    }
    const bool reservedDigit = (base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' ||
                               base[3] == L'\u00b2' || base[3] == L'\u00b3';
    return !reservedDigit ||
           !(::CompareStringOrdinal(base.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
             ::CompareStringOrdinal(base.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL);
}

[[nodiscard]] ULONG nativeDisposition(const RelativeOpenDisposition disposition) noexcept
{
    switch (disposition) {
    case RelativeOpenDisposition::OpenExisting:
        return FILE_OPEN;
    case RelativeOpenDisposition::CreateNew:
        return FILE_CREATE;
    case RelativeOpenDisposition::OpenOrCreate:
        return FILE_OPEN_IF;
    }
    return FILE_OPEN;
}

[[nodiscard]] RelativeOpenResult failure(const NTSTATUS status, const DWORD win32Error) noexcept
{
    return RelativeOpenResult{UniqueHandle{}, status, win32Error, 0U};
}

} // namespace

RelativeOpenResult openRelative(const HANDLE rootDirectory,
                                const std::wstring_view oneComponentName,
                                const RelativeOpenOptions& options) noexcept
{
    try {
        if (rootDirectory == nullptr || rootDirectory == INVALID_HANDLE_VALUE) {
            return failure(InvalidHandleStatus, ERROR_INVALID_HANDLE);
        }
        if (!validOneComponentName(oneComponentName)) {
            return failure(InvalidNameStatus, ERROR_INVALID_NAME);
        }

        const bool validDisposition =
            options.disposition == RelativeOpenDisposition::OpenExisting ||
            options.disposition == RelativeOpenDisposition::CreateNew ||
            options.disposition == RelativeOpenDisposition::OpenOrCreate;
        if (options.desiredAccess == 0U || !validDisposition ||
            (options.objectType != RelativeObjectType::File &&
             options.objectType != RelativeObjectType::Directory) ||
            (options.deleteOnClose && (options.desiredAccess & DELETE) == 0U) ||
            (options.shareAccess & ~(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)) !=
                0U) {
            return failure(InvalidParameterStatus, ERROR_INVALID_PARAMETER);
        }

        UNICODE_STRING name{};
        name.Length = static_cast<USHORT>(oneComponentName.size() * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        name.Buffer = const_cast<PWSTR>(oneComponentName.data());

        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, rootDirectory,
                                   nullptr);
        IO_STATUS_BLOCK ioStatus{};
        HANDLE rawHandle{};
        ULONG createOptions = options.objectType == RelativeObjectType::Directory
                                  ? FILE_DIRECTORY_FILE
                                  : FILE_NON_DIRECTORY_FILE;
        if (options.writeThrough) {
            createOptions |= FILE_WRITE_THROUGH;
        }
        if (options.sequentialAccess) {
            createOptions |= FILE_SEQUENTIAL_ONLY;
        }
        if (options.deleteOnClose) {
            createOptions |= FILE_DELETE_ON_CLOSE;
        }
        const NTSTATUS status = ::NtCreateFile(
            &rawHandle, options.desiredAccess | SYNCHRONIZE, &attributes, &ioStatus, nullptr,
            options.fileAttributes, options.shareAccess, nativeDisposition(options.disposition),
            createOptions | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0U);
        UniqueHandle handle{rawHandle};
        if (status < 0) {
            return failure(status, ::RtlNtStatusToDosError(status));
        }
        if (!handle) {
            return failure(InvalidHandleStatus, ERROR_INVALID_HANDLE);
        }
        return RelativeOpenResult{std::move(handle), status, ERROR_SUCCESS, ioStatus.Information};
    } catch (...) {
        return failure(NoMemoryStatus, ERROR_NOT_ENOUGH_MEMORY);
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
