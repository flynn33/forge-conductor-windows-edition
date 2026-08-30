#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <memory>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow native seam around the Windows registered-URI activation mechanism.
// The production implementation runs only in the bounded CLI helper. It calls
// the interactive Shell's local COM server so the Shell, rather than the helper
// job, owns any registered-browser process that must outlive activation.
class IWindowsDashboardUriLaunchPlatform {
public:
    virtual ~IWindowsDashboardUriLaunchPlatform() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> open(
        std::wstring_view uri) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IWindowsDashboardUriLaunchPlatform>
createWindowsDashboardUriLaunchPlatform();

} // namespace ForgeConductor::Infrastructure::Windows::Detail
