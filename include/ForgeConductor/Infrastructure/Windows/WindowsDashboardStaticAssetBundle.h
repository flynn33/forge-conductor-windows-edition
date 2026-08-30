#pragma once

#include "ForgeConductor/Dashboard/DashboardStaticAssetStore.h"
#include "ForgeConductor/Domain/Result.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

// Loads the dashboard resources embedded in the current process once and
// constructs the immutable application-owned store used by every listener
// generation. Runtime request lookup never reads the filesystem or resources.
class WindowsDashboardStaticAssetBundle final {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<Dashboard::DashboardStaticAssetStore>>
    create() noexcept;

    WindowsDashboardStaticAssetBundle() = delete;
};

} // namespace ForgeConductor::Infrastructure::Windows
