#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"

#include "Detail/UtfConversion.h"
#include "Detail/Win32Error.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::size_t MaximumNormalizedUtf16CodeUnits =
    Contracts::IUnicodeCanonicalizer::MaximumInputBytes;
constexpr std::size_t MaximumWriteAttempts = 2U;

[[nodiscard]] Domain::Error normalizationFailure(const DWORD nativeCode) noexcept
{
    if (nativeCode == ERROR_NO_UNICODE_TRANSLATION) {
        return Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Text could not be normalized because it contains invalid Unicode.");
    }
    return Detail::makeWin32Error(
        "normalize Unicode text to NFC", nativeCode);
}

[[nodiscard]] Domain::Result<Contracts::NfcUtf8Key> outputLimitFailure() noexcept
{
    return Domain::Result<Contracts::NfcUtf8Key>::failure(Domain::makeError(
        Domain::ErrorCodes::PayloadTooLarge,
        "Unicode canonicalization output exceeds its bounded UTF-8 key limit."));
}

} // namespace

Domain::Result<Contracts::NfcUtf8Key>
WindowsUnicodeCanonicalizer::nfcKey(const std::string_view value) const noexcept
{
    try {
        if (value.size() > MaximumInputBytes) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Unicode canonicalization input exceeds 1 MiB."));
        }

        auto utf16 = Detail::strictUtf8ToUtf16(value);
        if (!utf16) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                std::move(utf16).error());
        }
        if (utf16.value().empty()) {
            return Contracts::NfcUtf8Key::create(std::string{});
        }

        const int inputLength = static_cast<int>(utf16.value().size());
        ::SetLastError(ERROR_SUCCESS);
        const int estimated = ::NormalizeString(
            NormalizationC,
            utf16.value().data(),
            inputLength,
            nullptr,
            0);
        if (estimated <= 0) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                normalizationFailure(::GetLastError()));
        }
        if (static_cast<std::size_t>(estimated) >
            MaximumNormalizedUtf16CodeUnits) {
            return outputLimitFailure();
        }

        std::size_t capacity = static_cast<std::size_t>(estimated);
        std::wstring normalized;
        bool completed = false;
        for (std::size_t attempt = 0; attempt < MaximumWriteAttempts; ++attempt) {
            normalized.assign(capacity, L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const int written = ::NormalizeString(
                NormalizationC,
                utf16.value().data(),
                inputLength,
                normalized.data(),
                static_cast<int>(normalized.size()));
            if (written > 0) {
                normalized.resize(static_cast<std::size_t>(written));
                completed = true;
                break;
            }

            const DWORD nativeCode = ::GetLastError();
            if (nativeCode != ERROR_INSUFFICIENT_BUFFER ||
                attempt + 1U == MaximumWriteAttempts) {
                return Domain::Result<Contracts::NfcUtf8Key>::failure(
                    normalizationFailure(nativeCode));
            }

            const std::int64_t signedWritten = written;
            const std::uint64_t suggested = signedWritten < 0
                ? static_cast<std::uint64_t>(-signedWritten)
                : 0U;
            if (suggested == 0U ||
                suggested > MaximumNormalizedUtf16CodeUnits) {
                return outputLimitFailure();
            }
            capacity = static_cast<std::size_t>(suggested);
        }
        if (!completed) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Unicode canonicalization exhausted its bounded write attempts."));
        }

        auto utf8 = Detail::strictUtf16ToUtf8(normalized);
        if (!utf8) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                std::move(utf8).error());
        }
        if (utf8.value().size() > MaximumKeyBytes) {
            return outputLimitFailure();
        }
        return Contracts::NfcUtf8Key::create(std::move(utf8).value());
    } catch (...) {
        return Domain::Result<Contracts::NfcUtf8Key>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Unicode canonicalization could not allocate its bounded output."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
