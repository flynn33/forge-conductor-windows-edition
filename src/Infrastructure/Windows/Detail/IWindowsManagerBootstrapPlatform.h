#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class ManagerBootstrapLaunchMode { DesktopShell, DetachedManager };

// The desktop Shell launches the current CLI; that independent process starts
// its exact Manager sibling. Neither mode accepts an executable or extra args.
class IWindowsManagerBootstrapPlatform {
public:
    virtual ~IWindowsManagerBootstrapPlatform() noexcept = default;
    [[nodiscard]] virtual Domain::Result<void> start(
        const Domain::PathText& home, bool isolatedProfile,
        ManagerBootstrapLaunchMode mode = ManagerBootstrapLaunchMode::DesktopShell) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IWindowsManagerBootstrapPlatform>
createWindowsManagerBootstrapPlatform();
}
