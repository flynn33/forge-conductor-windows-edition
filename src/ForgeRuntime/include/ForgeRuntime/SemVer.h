// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace Forge::Runtime {

struct SemVer final {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};
    std::optional<std::string> prerelease;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] static SemVer parse(const std::string& text);
    [[nodiscard]] int compare(const SemVer& other) const noexcept;

    [[nodiscard]] bool operator==(const SemVer&) const = default;
    [[nodiscard]] bool operator<(const SemVer& other) const noexcept { return compare(other) < 0; }
    [[nodiscard]] bool operator<=(const SemVer& other) const noexcept { return compare(other) <= 0; }
    [[nodiscard]] bool operator>(const SemVer& other) const noexcept { return compare(other) > 0; }
};

} // namespace Forge::Runtime
