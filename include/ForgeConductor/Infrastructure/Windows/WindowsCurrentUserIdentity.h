#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsCurrentUserIdentity final {
public:
    static constexpr std::size_t MaximumTokenUserInformationBytes = 4U * 1024U;
    static constexpr std::size_t MaximumSidBytes = 68U;
    static constexpr std::size_t StableKeyCharacters = 64U;

    [[nodiscard]] static Domain::Result<WindowsCurrentUserIdentity> load() noexcept;

    [[nodiscard]] std::span<const std::byte> sidBytes() const noexcept
    {
        return sidBytes_;
    }

    [[nodiscard]] const std::string& sidText() const noexcept { return sidText_; }
    [[nodiscard]] const std::string& stableKey() const noexcept { return stableKey_; }

private:
    WindowsCurrentUserIdentity(
        std::vector<std::byte> sidBytes,
        std::string sidText,
        std::string stableKey) noexcept;

    std::vector<std::byte> sidBytes_;
    std::string sidText_;
    std::string stableKey_;
};

} // namespace ForgeConductor::Infrastructure::Windows
