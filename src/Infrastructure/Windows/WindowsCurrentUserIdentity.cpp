#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UniqueLocalAllocation.h"
#include "Detail/Win32Error.h"

#include <Windows.h>
#include <sddl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view IdentityHashDomain =
    "ForgeConductor.WindowsCurrentUserIdentity.v1";
constexpr std::size_t MaximumSidTextCharacters = 256U;
constexpr std::size_t MinimumSidBytes = 8U;

[[nodiscard]] Domain::Result<void> validateSidText(
    const std::string_view value) noexcept
{
    try {
        if (!value.starts_with("S-1-") ||
            value.size() > MaximumSidTextCharacters) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Windows returned a non-canonical current-user SID string."));
        }
        for (std::size_t index = 2U; index < value.size(); ++index) {
            const char character = value[index];
            if ((character < '0' || character > '9') && character != '-') {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned a current-user SID string with invalid characters."));
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The current-user SID string could not be validated."));
    }
}

[[nodiscard]] Domain::Result<std::string> stableIdentityKey(
    const std::span<const std::byte> sidBytes) noexcept
{
    try {
        if (sidBytes.size() > WindowsCurrentUserIdentity::MaximumSidBytes) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The current-user SID exceeded the stable-identity input bound."));
        }

        // Hash a domain separator, a NUL delimiter, a fixed-width big-endian
        // length, and the exact SID bytes. The framing prevents concatenation
        // ambiguity while keeping the resulting identity independent of locale.
        std::vector<std::byte> hashInput;
        hashInput.reserve(
            IdentityHashDomain.size() + 1U + sizeof(std::uint32_t) + sidBytes.size());
        const auto domainBytes = std::as_bytes(std::span{
            IdentityHashDomain.data(), IdentityHashDomain.size()});
        hashInput.insert(hashInput.end(), domainBytes.begin(), domainBytes.end());
        hashInput.push_back(std::byte{0});

        const auto sidLength = static_cast<std::uint32_t>(sidBytes.size());
        hashInput.push_back(static_cast<std::byte>((sidLength >> 24U) & 0xffU));
        hashInput.push_back(static_cast<std::byte>((sidLength >> 16U) & 0xffU));
        hashInput.push_back(static_cast<std::byte>((sidLength >> 8U) & 0xffU));
        hashInput.push_back(static_cast<std::byte>(sidLength & 0xffU));
        hashInput.insert(hashInput.end(), sidBytes.begin(), sidBytes.end());

        BCryptSha256Hasher hasher;
        auto digest = hasher.sha256(std::span<const std::byte>{hashInput});
        if (!digest) {
            return Domain::Result<std::string>::failure(std::move(digest).error());
        }
        return Domain::Result<std::string>::success(digest.value().value());
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The stable current-user identity key could not allocate bounded state."));
    }
}

} // namespace

WindowsCurrentUserIdentity::WindowsCurrentUserIdentity(
    std::vector<std::byte> sidBytes,
    std::string sidText,
    std::string stableKey) noexcept
    : sidBytes_{std::move(sidBytes)},
      sidText_{std::move(sidText)},
      stableKey_{std::move(stableKey)}
{
}

Domain::Result<WindowsCurrentUserIdentity>
WindowsCurrentUserIdentity::load() noexcept
{
    try {
        HANDLE rawToken = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Detail::makeWin32Error(
                    "open the current process token", ::GetLastError(),
                    Domain::ErrorCodes::Unauthorized));
        }
        Detail::UniqueHandle token{rawToken};

        DWORD requiredBytes = 0U;
        ::SetLastError(ERROR_SUCCESS);
        const BOOL sized = ::GetTokenInformation(
            token.get(), TokenUser, nullptr, 0U, &requiredBytes);
        const DWORD sizingError = ::GetLastError();
        if (sized != FALSE || sizingError != ERROR_INSUFFICIENT_BUFFER) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Detail::makeWin32Error(
                    "size the current-user token identity", sizingError,
                    Domain::ErrorCodes::IntegrityFailure));
        }
        if (requiredBytes < sizeof(TOKEN_USER) ||
            requiredBytes > MaximumTokenUserInformationBytes) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows reported an unsafe current-user token identity size."));
        }

        auto* const rawInformation = static_cast<std::byte*>(
            ::LocalAlloc(LPTR, requiredBytes));
        if (rawInformation == nullptr) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Detail::makeWin32Error(
                    "allocate bounded current-user token identity storage",
                    ERROR_NOT_ENOUGH_MEMORY));
        }
        Detail::UniqueLocalAllocation<std::byte> information{rawInformation};

        DWORD returnedBytes = 0U;
        if (::GetTokenInformation(
                token.get(), TokenUser, information.get(), requiredBytes,
                &returnedBytes) == FALSE) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Detail::makeWin32Error(
                    "read the current-user token identity", ::GetLastError(),
                    Domain::ErrorCodes::Unauthorized));
        }
        if (returnedBytes < sizeof(TOKEN_USER) || returnedBytes > requiredBytes) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned an inconsistent current-user token identity size."));
        }

        const auto* const tokenUser =
            reinterpret_cast<const TOKEN_USER*>(information.get());
        const PSID rawSid = tokenUser->User.Sid;
        const std::uintptr_t bufferBegin =
            reinterpret_cast<std::uintptr_t>(information.get());
        const std::uintptr_t bufferEnd = bufferBegin + returnedBytes;
        const std::uintptr_t sidBegin = reinterpret_cast<std::uintptr_t>(rawSid);
        if (rawSid == nullptr || sidBegin < bufferBegin || sidBegin > bufferEnd ||
            (bufferEnd - sidBegin) < MinimumSidBytes) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned an out-of-range current-user SID."));
        }
        if (::IsValidSid(rawSid) == FALSE) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned an invalid current-user SID."));
        }

        const DWORD sidLength = ::GetLengthSid(rawSid);
        if (sidLength < MinimumSidBytes || sidLength > MaximumSidBytes ||
            static_cast<std::uintptr_t>(sidLength) > (bufferEnd - sidBegin)) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned a current-user SID outside its bounded token storage."));
        }

        std::vector<std::byte> sidBytes(static_cast<std::size_t>(sidLength));
        std::memcpy(sidBytes.data(), rawSid, sidBytes.size());

        LPSTR rawSidText = nullptr;
        if (::ConvertSidToStringSidA(
                reinterpret_cast<PSID>(sidBytes.data()), &rawSidText) == FALSE) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Detail::makeWin32Error(
                    "format the current-user SID", ::GetLastError(),
                    Domain::ErrorCodes::IntegrityFailure));
        }
        Detail::UniqueLocalAllocation<char> sidTextOwner{rawSidText};
        const std::size_t sidTextLength =
            ::strnlen_s(rawSidText, MaximumSidTextCharacters + 1U);
        if (sidTextLength == 0U || sidTextLength > MaximumSidTextCharacters) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Windows returned an unbounded current-user SID string."));
        }
        std::string sidText{rawSidText, sidTextLength};
        auto validText = validateSidText(sidText);
        if (!validText) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                std::move(validText).error());
        }

        auto stableKey = stableIdentityKey(sidBytes);
        if (!stableKey) {
            return Domain::Result<WindowsCurrentUserIdentity>::failure(
                std::move(stableKey).error());
        }
        return Domain::Result<WindowsCurrentUserIdentity>::success(
            WindowsCurrentUserIdentity{
                std::move(sidBytes), std::move(sidText),
                std::move(stableKey).value()});
    } catch (...) {
        return Domain::Result<WindowsCurrentUserIdentity>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The current-user identity could not allocate bounded state."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
