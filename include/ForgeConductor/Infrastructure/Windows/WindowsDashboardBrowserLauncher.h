#pragma once

#include "ForgeConductor/Contracts/IDashboardBrowserLauncher.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"
#include "ForgeConductor/Domain/Identifiers.h"

#include <chrono>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
struct WindowsDashboardBrowserLauncherTestAccess;
} // namespace Detail

struct WindowsDashboardBrowserLaunchConfiguration final {
    Domain::PathText managerExecutable;
    Domain::PathText helperExecutable;
    Contracts::WorkspaceAuthority helperAuthority;
    std::chrono::milliseconds helperTimeout{std::chrono::seconds{5}};
};

// One-shot process-owned launcher for the browser-on-manager-start behavior.
// It owns a capacity-one jthread and runs the exact sibling CLI helper through
// the bounded process supervisor. The authenticated URI crosses only inherited
// stdin; it never appears in argv or the child environment. The helper performs
// Shell-owned activation so the registered browser can outlive the helper job.
class WindowsDashboardBrowserLauncher final
    : public Contracts::IDashboardBrowserLauncher {
public:
    static constexpr std::chrono::milliseconds MaximumHelperTimeout{
        std::chrono::seconds{15}};

    WindowsDashboardBrowserLauncher(
        Contracts::IClock& clock,
        Contracts::IDiagnosticSink& diagnostics,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
        WindowsDashboardBrowserLaunchConfiguration configuration,
        Domain::Sha256Digest bearerToken);
    ~WindowsDashboardBrowserLauncher() noexcept override;

    WindowsDashboardBrowserLauncher(
        const WindowsDashboardBrowserLauncher&) = delete;
    WindowsDashboardBrowserLauncher& operator=(
        const WindowsDashboardBrowserLauncher&) = delete;
    WindowsDashboardBrowserLauncher(
        WindowsDashboardBrowserLauncher&&) = delete;
    WindowsDashboardBrowserLauncher& operator=(
        WindowsDashboardBrowserLauncher&&) = delete;

    [[nodiscard]] Domain::Result<void> launch(
        std::string_view dashboardHost,
        std::uint16_t dashboardPort,
        const Domain::OperationContext& context) noexcept override;

    void beginShutdown() noexcept override;
    void shutdown() noexcept override;

private:
    friend struct Detail::WindowsDashboardBrowserLauncherTestAccess;

    [[nodiscard]] bool closedForTesting() const noexcept;
    [[nodiscard]] bool waitUntilJoinOwnedForTesting(
        std::chrono::milliseconds timeout) noexcept;

    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
