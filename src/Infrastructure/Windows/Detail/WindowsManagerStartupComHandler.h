#pragma once

#include "ManagerStartupComWorker.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class IWindowsTaskSchedulerStartupPlatform;

// Applies the product-owned startup state machine on the worker MTA. The
// platform remains a projection/mutation boundary and never decides ownership.
class WindowsManagerStartupComHandler final
    : public IManagerStartupComHandler {
public:
    explicit WindowsManagerStartupComHandler(
        std::shared_ptr<IWindowsTaskSchedulerStartupPlatform> platform);

    [[nodiscard]] ManagerStartupComResult handle(
        const ManagerStartupComRequest& request) noexcept override;

private:
    std::shared_ptr<IWindowsTaskSchedulerStartupPlatform> platform_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
