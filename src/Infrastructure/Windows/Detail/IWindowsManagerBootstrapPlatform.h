#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Launch only the current CLI's exact Manager sibling through the desktop
// Shell. It accepts no executable, arbitrary arguments, verb or environment.
class IWindowsManagerBootstrapPlatform {
public:
    virtual ~IWindowsManagerBootstrapPlatform() noexcept = default;
    [[nodiscard]] virtual Domain::Result<void> start(
        const Domain::PathText& home, bool isolatedProfile) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IWindowsManagerBootstrapPlatform>
createWindowsManagerBootstrapPlatform();
}
