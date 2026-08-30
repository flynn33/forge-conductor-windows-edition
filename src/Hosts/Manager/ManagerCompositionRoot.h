#pragma once

#include "ManagerProcessEnvironment.h"

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"

#include <memory>
#include <optional>
#include <string>

namespace ForgeConductor::Hosts::Manager {

struct ManagerCompositionRootOptions final {
    // Production leaves this empty. The Task Scheduler --home argument is
    // carried here only as an equality assertion against the independently
    // resolved current-user application root.
    std::optional<Domain::PathText> expectedHome;

    // Production uses the default empty values. Purpose-scoped native process
    // fixtures may inject an isolated data root, instance suffix, and DPAPI
    // registry subkey without changing the production argument contract.
    Composition::Windows::ManagerProcessEnvironmentOptions environment;
    Infrastructure::Windows::WindowsManagerInstanceLeaseOptions instanceLease;
    std::optional<std::wstring> secureStorageRegistrySubkey;

    // Production leaves this enabled. Controlled real-process fixtures may
    // disable external-host discovery so isolated Manager lifecycle evidence
    // cannot inspect or repair the operator's LM Studio installation.
    bool enableExternalHostMaintenance{true};

    bool openBrowserOverride{};
    bool enableEtw{true};
};

// Final process-owned composition boundary for ForgeConductor.Manager.exe.
// create() performs read-only environment inspection before acquiring the
// per-user lease, and the private implementation retains that lease until
// every ingress, worker, repository, and borrowed service has shut down.
class ManagerCompositionRoot final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<ManagerCompositionRoot>>
    create(ManagerCompositionRootOptions options = {}) noexcept;

    ~ManagerCompositionRoot() noexcept;

    ManagerCompositionRoot(const ManagerCompositionRoot&) = delete;
    ManagerCompositionRoot& operator=(const ManagerCompositionRoot&) = delete;
    ManagerCompositionRoot(ManagerCompositionRoot&&) = delete;
    ManagerCompositionRoot& operator=(ManagerCompositionRoot&&) = delete;

    [[nodiscard]] Domain::Result<void> run() noexcept;
    void shutdown() noexcept;

private:
    class Impl;

    explicit ManagerCompositionRoot(std::unique_ptr<Impl> implementation)
        noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Hosts::Manager
