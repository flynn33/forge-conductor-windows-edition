#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Contracts {

class IAgentCatalog;
class IAgentSessionRepository;
class IClock;
class ILMStudioDeploymentService;
class ITelemetryService;

} // namespace ForgeConductor::Contracts

namespace ForgeConductor::Composition::Windows {

// Bounded, immutable-at-return evidence gathered directly from Windows. The
// production probe owns no persistent OS resource; all handles are scoped to
// inspect().
struct ManagerDoctorPlatformSnapshot final {
    bool homeDirectoryPresent{};
    bool centralStorePresent{};
    std::optional<Domain::PathText> gitExecutablePath;
    bool installedBinaryExecutable{};
    std::vector<std::string> legacyLauncherBasenames;
};

class IManagerDoctorPlatformProbe {
public:
    virtual ~IManagerDoctorPlatformProbe() noexcept = default;

    [[nodiscard]] virtual Domain::Result<ManagerDoctorPlatformSnapshot> inspect(
        const Domain::PathText& dataRoot,
        const Domain::PathText& centralStorePath,
        const Domain::PathText& installedBinaryPath,
        const Domain::OperationContext& context) noexcept = 0;
};

class WindowsManagerDoctorPlatformProbe final
    : public IManagerDoctorPlatformProbe {
public:
    explicit WindowsManagerDoctorPlatformProbe(
        const Contracts::IClock& clock) noexcept;
    ~WindowsManagerDoctorPlatformProbe() noexcept override = default;

    WindowsManagerDoctorPlatformProbe(
        const WindowsManagerDoctorPlatformProbe&) = delete;
    WindowsManagerDoctorPlatformProbe& operator=(
        const WindowsManagerDoctorPlatformProbe&) = delete;
    WindowsManagerDoctorPlatformProbe(
        WindowsManagerDoctorPlatformProbe&&) = delete;
    WindowsManagerDoctorPlatformProbe& operator=(
        WindowsManagerDoctorPlatformProbe&&) = delete;

    [[nodiscard]] Domain::Result<ManagerDoctorPlatformSnapshot> inspect(
        const Domain::PathText& dataRoot,
        const Domain::PathText& centralStorePath,
        const Domain::PathText& installedBinaryPath,
        const Domain::OperationContext& context) noexcept override;

private:
    const Contracts::IClock& clock_;
};

struct ManagerDoctorServiceConfiguration final {
    std::string productVersion;
    Domain::PathText dataRoot;
    Domain::PathText centralStorePath;
    Domain::PathText installedBinaryPath;
    Domain::ResourceBudgets resourceBudgets;
    Contracts::WorkspaceAuthority lmStudioReadAuthority;
};

// Application-owned production Doctor implementation. Dependencies are
// borrowed from the Manager composition root and outlive this service.
class ManagerDoctorService final : public Contracts::IDoctorService {
public:
    static constexpr std::size_t RequiredAgentCount = 10U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<ManagerDoctorService>>
    create(
        ManagerDoctorServiceConfiguration configuration,
        IManagerDoctorPlatformProbe& platformProbe,
        Contracts::IAgentCatalog& agentCatalog,
        Contracts::IAgentSessionRepository& sessionRepository,
        Contracts::ITelemetryService& telemetryService,
        Contracts::ILMStudioDeploymentService& lmStudioDeployment,
        const Contracts::IClock& clock) noexcept;

    ~ManagerDoctorService() noexcept override;

    ManagerDoctorService(const ManagerDoctorService&) = delete;
    ManagerDoctorService& operator=(const ManagerDoctorService&) = delete;
    ManagerDoctorService(ManagerDoctorService&&) = delete;
    ManagerDoctorService& operator=(ManagerDoctorService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::DoctorReport> run(
        const Domain::OperationContext& context) noexcept override;

    // Stops new admissions and drains active run() calls. Borrowed services are
    // deliberately not shut down here; the composition root owns their order.
    void shutdown() noexcept override;

private:
    class Impl;

    explicit ManagerDoctorService(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Composition::Windows
