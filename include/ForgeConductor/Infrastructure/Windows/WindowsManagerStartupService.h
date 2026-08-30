#pragma once

#include "ForgeConductor/Contracts/IManagerStartupService.h"

#include <memory>
#include <string>

namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
class IWindowsTaskSchedulerStartupPlatform;
struct WindowsManagerStartupServiceTestAccess;
} // namespace Detail

struct WindowsManagerStartupServiceOptions final {
    // Empty in production. Focused machine tests use a safe suffix so they
    // cannot collide with the real per-user registration.
    std::string purposeSuffix;
};

class WindowsManagerStartupService final
    : public Contracts::IManagerStartupService {
public:
    explicit WindowsManagerStartupService(
        WindowsManagerStartupServiceOptions options = {});
    ~WindowsManagerStartupService() override;

    WindowsManagerStartupService(const WindowsManagerStartupService&) = delete;
    WindowsManagerStartupService& operator=(
        const WindowsManagerStartupService&) = delete;
    WindowsManagerStartupService(WindowsManagerStartupService&&) = delete;
    WindowsManagerStartupService& operator=(
        WindowsManagerStartupService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupStatus> inspect(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome>
    registerAtLogon(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome> repair(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome> setEnabled(
        const Domain::ManagerStartupDefinition& expected,
        bool enabled,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome> startNow(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome> remove(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    friend struct Detail::WindowsManagerStartupServiceTestAccess;

    WindowsManagerStartupService(
        WindowsManagerStartupServiceOptions options,
        std::shared_ptr<Detail::IWindowsTaskSchedulerStartupPlatform> platform);

    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
