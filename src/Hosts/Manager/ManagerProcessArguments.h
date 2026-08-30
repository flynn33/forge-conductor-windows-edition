#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace ForgeConductor::Hosts::Manager {

// Process arguments are parsed at the native UTF-16 host boundary. The home
// value is an equality assertion against the independently resolved production
// root; it is never an input to root selection.
struct ManagerProcessArguments final {
    static constexpr std::size_t MaximumInspectedArgumentCount = 4U;
    static constexpr std::size_t MaximumHomeUtf16Units = 32'767U;

    std::optional<Domain::PathText> expectedHome;
    bool openBrowser{};

    bool operator==(const ManagerProcessArguments&) const = default;

    [[nodiscard]] static Domain::Result<ManagerProcessArguments> parse(
        std::span<const std::wstring_view> arguments) noexcept;
};

} // namespace ForgeConductor::Hosts::Manager
