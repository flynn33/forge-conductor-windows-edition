#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <istream>
#include <memory>
#include <span>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
class IWindowsManagerBootstrapPlatform;
struct WindowsManagerBootstrapCommandTestAccess;
}

// Fixed internal CLI command; executable selection and launch arguments remain
// native. The connector bounds the helper process independently of Shell COM.
class WindowsManagerBootstrapCommand final {
public:
    static constexpr std::size_t MaximumRequestBytes = 64U * 1024U;
    WindowsManagerBootstrapCommand();
    ~WindowsManagerBootstrapCommand() noexcept;
    WindowsManagerBootstrapCommand(const WindowsManagerBootstrapCommand&) = delete;
    WindowsManagerBootstrapCommand& operator=(const WindowsManagerBootstrapCommand&) = delete;
    [[nodiscard]] Domain::Result<void> run(std::istream& input) noexcept;
    [[nodiscard]] Domain::Result<void> launch(
        std::span<const std::string_view> arguments) noexcept;

private:
    friend struct Detail::WindowsManagerBootstrapCommandTestAccess;
    explicit WindowsManagerBootstrapCommand(
        std::unique_ptr<Detail::IWindowsManagerBootstrapPlatform> platform);
    const std::unique_ptr<Detail::IWindowsManagerBootstrapPlatform> platform_;
};
}
