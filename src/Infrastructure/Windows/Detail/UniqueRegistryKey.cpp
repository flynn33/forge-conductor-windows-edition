#include "UniqueRegistryKey.h"

#include "Win32Error.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::wstring_view RequiredPrefix = L"Software\\Forge Conductor\\";
constexpr std::size_t MaximumRegistrySubkeyCharacters = 512U;

[[nodiscard]] bool equalsCaseInsensitive(
    const wchar_t left,
    const wchar_t right) noexcept
{
    return std::towlower(left) == std::towlower(right);
}

[[nodiscard]] bool startsWithCaseInsensitive(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept
{
    return value.size() >= prefix.size() &&
        std::equal(
            prefix.begin(),
            prefix.end(),
            value.begin(),
            equalsCaseInsensitive);
}

} // namespace

Domain::Result<void> validateForgeRegistrySubkey(
    const std::wstring_view subkey) noexcept
{
    try {
        if (subkey.empty() ||
            subkey.size() > MaximumRegistrySubkeyCharacters ||
            !startsWithCaseInsensitive(subkey, RequiredPrefix) ||
            subkey.back() == L'\\' ||
            subkey.find(L'\0') != std::wstring_view::npos ||
            subkey.find(L"\\\\") != std::wstring_view::npos ||
            subkey.find(L"\\.\\") != std::wstring_view::npos ||
            subkey.find(L"\\..\\") != std::wstring_view::npos) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Secure-storage registry scope is not an app-owned HKCU subkey."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Secure-storage registry scope validation failed."));
    }
}

Domain::Result<UniqueRegistryKey> UniqueRegistryKey::createCurrentUser(
    const std::wstring_view subkey,
    const REGSAM access) noexcept
{
    auto valid = validateForgeRegistrySubkey(subkey);
    if (!valid) {
        return Domain::Result<UniqueRegistryKey>::failure(
            std::move(valid).error());
    }

    HKEY key{};
    const std::wstring terminated{subkey};
    const auto status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        terminated.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        access,
        nullptr,
        &key,
        nullptr);
    if (status != ERROR_SUCCESS) {
        return Domain::Result<UniqueRegistryKey>::failure(makeWin32Error(
            "Create secure-storage registry key",
            static_cast<DWORD>(status)));
    }
    return Domain::Result<UniqueRegistryKey>::success(UniqueRegistryKey{key});
}

Domain::Result<std::optional<UniqueRegistryKey>>
UniqueRegistryKey::openCurrentUser(
    const std::wstring_view subkey,
    const REGSAM access) noexcept
{
    auto valid = validateForgeRegistrySubkey(subkey);
    if (!valid) {
        return Domain::Result<std::optional<UniqueRegistryKey>>::failure(
            std::move(valid).error());
    }

    HKEY key{};
    const std::wstring terminated{subkey};
    const auto status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        terminated.c_str(),
        0,
        access,
        &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return Domain::Result<std::optional<UniqueRegistryKey>>::success(
            std::nullopt);
    }
    if (status != ERROR_SUCCESS) {
        return Domain::Result<std::optional<UniqueRegistryKey>>::failure(
            makeWin32Error(
                "Open secure-storage registry key",
                static_cast<DWORD>(status)));
    }
    return Domain::Result<std::optional<UniqueRegistryKey>>::success(
        std::optional<UniqueRegistryKey>{UniqueRegistryKey{key}});
}

UniqueRegistryKey::UniqueRegistryKey(UniqueRegistryKey&& other) noexcept
    : key_{std::exchange(other.key_, nullptr)}
{
}

UniqueRegistryKey& UniqueRegistryKey::operator=(UniqueRegistryKey&& other) noexcept
{
    if (this != &other) {
        reset(std::exchange(other.key_, nullptr));
    }
    return *this;
}

UniqueRegistryKey::~UniqueRegistryKey()
{
    reset();
}

void UniqueRegistryKey::reset(const HKEY replacement) noexcept
{
    if (key_ != nullptr) {
        RegCloseKey(key_);
    }
    key_ = replacement;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
