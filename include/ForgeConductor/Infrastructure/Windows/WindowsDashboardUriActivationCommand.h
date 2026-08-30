#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <istream>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
class IWindowsDashboardUriLaunchPlatform;
struct WindowsDashboardUriActivationCommandTestAccess;
} // namespace Detail

// Internal CLI helper command. It consumes one exact authenticated loopback
// dashboard URI from bounded stdin and delegates activation to the interactive
// Windows Shell. The URI is never accepted from argv or the environment.
class WindowsDashboardUriActivationCommand final {
public:
    static constexpr std::size_t MaximumUriBytes = 128U;

    WindowsDashboardUriActivationCommand();
    ~WindowsDashboardUriActivationCommand() noexcept;

    WindowsDashboardUriActivationCommand(
        const WindowsDashboardUriActivationCommand&) = delete;
    WindowsDashboardUriActivationCommand& operator=(
        const WindowsDashboardUriActivationCommand&) = delete;
    WindowsDashboardUriActivationCommand(
        WindowsDashboardUriActivationCommand&&) = delete;
    WindowsDashboardUriActivationCommand& operator=(
        WindowsDashboardUriActivationCommand&&) = delete;

    [[nodiscard]] Domain::Result<void> run(std::istream& input) noexcept;

private:
    friend struct Detail::WindowsDashboardUriActivationCommandTestAccess;

    explicit WindowsDashboardUriActivationCommand(
        std::unique_ptr<Detail::IWindowsDashboardUriLaunchPlatform> platform);

    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
