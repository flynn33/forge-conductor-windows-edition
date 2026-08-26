#include "Win32Error.h"

#include "UniqueLocalAllocation.h"

#include <limits>
#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {

[[nodiscard]] std::string systemMessageToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
            return {};
        }
        const int inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return {};
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required, nullptr, nullptr) != required) {
            return {};
        }
        return converted;
    } catch (...) {
        return {};
    }
}
namespace {

[[nodiscard]] std::string trimSystemMessage(std::string value)
{
    while (!value.empty()) {
        const char last = value.back();
        if (last != '\r' && last != '\n' && last != ' ' && last != '\t' &&
            last != '.') {
            break;
        }
        value.pop_back();
    }
    return value;
}

} // namespace

Domain::Error makeWin32Error(
    const std::string_view action,
    const DWORD nativeCode,
    const std::string_view stableCode,
    const bool retryable) noexcept
{
    try {
        wchar_t* rawMessage = nullptr;
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            nativeCode,
            0,
            reinterpret_cast<wchar_t*>(&rawMessage),
            0,
            nullptr);
        UniqueLocalAllocation<wchar_t> messageOwner{rawMessage};

        std::string message{action};
        message += " failed with Win32 error ";
        message += std::to_string(nativeCode);
        if (messageLength != 0 && rawMessage != nullptr) {
            const std::string detail = trimSystemMessage(systemMessageToUtf8(
                std::wstring_view{
                    rawMessage, static_cast<std::size_t>(messageLength)}));
            if (!detail.empty()) {
                message += " (";
                message += detail;
                message += ')';
            }
        }
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

Domain::Error makeNtStatusError(
    const std::string_view action,
    const NTSTATUS nativeStatus,
    const std::string_view stableCode,
    const bool retryable) noexcept
{
    try {
        std::string message{action};
        message += " failed with NTSTATUS ";
        message += std::to_string(static_cast<long>(nativeStatus));
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows cryptographic operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
