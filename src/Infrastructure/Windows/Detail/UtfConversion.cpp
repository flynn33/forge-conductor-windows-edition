#include "UtfConversion.h"

#include "Win32Error.h"

#include <Windows.h>

#include <limits>
#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

template <typename T>
[[nodiscard]] Domain::Result<T> conversionFailure(
    const DWORD nativeCode,
    const char* direction) noexcept
{
    if (nativeCode == ERROR_NO_UNICODE_TRANSLATION) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Text is not valid "} + direction + "."));
    }
    return Domain::Result<T>::failure(makeWin32Error(
        std::string{"convert "} + direction,
        nativeCode));
}

} // namespace

Domain::Result<std::wstring> strictUtf8ToUtf16(
    const std::string_view value) noexcept
{
    try {
        if (value.empty()) {
            return Domain::Result<std::wstring>::success({});
        }
        if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "UTF-8 input exceeds the Windows conversion limit."));
        }

        const int inputLength = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            nullptr,
            0);
        if (required == 0) {
            return conversionFailure<std::wstring>(::GetLastError(), "UTF-8");
        }

        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        const int written = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            converted.data(),
            required);
        if (written != required) {
            return conversionFailure<std::wstring>(::GetLastError(), "UTF-8");
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "UTF-8 conversion could not allocate its bounded output."));
    }
}

Domain::Result<std::string> strictUtf16ToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty()) {
            return Domain::Result<std::string>::success({});
        }
        if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "UTF-16 input exceeds the Windows conversion limit."));
        }

        const int inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required == 0) {
            return conversionFailure<std::string>(::GetLastError(), "UTF-16");
        }

        std::string converted(static_cast<std::size_t>(required), '\0');
        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            converted.data(),
            required,
            nullptr,
            nullptr);
        if (written != required) {
            return conversionFailure<std::string>(::GetLastError(), "UTF-16");
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "UTF-16 conversion could not allocate its bounded output."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
