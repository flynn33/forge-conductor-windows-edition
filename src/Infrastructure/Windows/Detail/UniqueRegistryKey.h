#pragma once

#include "ForgeConductor/Domain/Result.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Owns one HKEY created/opened under HKCU. The creating/opening thread has no
// affinity requirement; destruction closes the handle after all serialized use.
class UniqueRegistryKey final {
public:
    [[nodiscard]] static Domain::Result<UniqueRegistryKey> createCurrentUser(
        std::wstring_view subkey,
        REGSAM access) noexcept;

    [[nodiscard]] static Domain::Result<std::optional<UniqueRegistryKey>>
    openCurrentUser(std::wstring_view subkey, REGSAM access) noexcept;

    UniqueRegistryKey(const UniqueRegistryKey&) = delete;
    UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;
    UniqueRegistryKey(UniqueRegistryKey&& other) noexcept;
    UniqueRegistryKey& operator=(UniqueRegistryKey&& other) noexcept;
    ~UniqueRegistryKey();

    [[nodiscard]] HKEY get() const noexcept { return key_; }

private:
    explicit UniqueRegistryKey(HKEY key) noexcept : key_{key} {}
    void reset(HKEY replacement = nullptr) noexcept;

    HKEY key_{};
};

[[nodiscard]] Domain::Result<void> validateForgeRegistrySubkey(
    std::wstring_view subkey) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
