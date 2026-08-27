#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/ILMStudioEnvironment.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

// A discovery candidate represents exactly one resource: either an application
// executable or a configuration file. Splitting the resources keeps evidence
// truthful when, for example, an installed executable exists but mcp.json is
// missing or malformed.
struct WindowsLMStudioDiscoveryCandidate final {
    Domain::LMStudioDiscoverySource source;
    Domain::PathText evidencePath;
    std::optional<Domain::PathText> applicationExecutable;
    std::optional<Domain::PathText> configurationPath;
    std::optional<Domain::PathText> installationRoot;
    std::optional<std::string> version;
    bool valid{};
    std::string detail;
};

class IWindowsLMStudioDiscoverySource {
public:
    virtual ~IWindowsLMStudioDiscoverySource() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>
    discover(const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

struct WindowsLMStudioDiscoverySourceOptions final {
    static constexpr std::size_t DefaultMaximumCandidates = 64U;

    std::optional<Domain::PathText> userProfileRoot;
    std::optional<Domain::PathText> localApplicationDataRoot;
    std::size_t maximumCandidates{DefaultMaximumCandidates};
    bool inspectInstalledApplicationRegistrations{true};
    bool inspectKnownUserLocations{true};
    bool inspectRunningProcesses{true};
};

// Native, read-only source for installed-application registrations, known
// per-user locations, and running process image paths. It never opens or
// automates the LM Studio UI.
class WindowsLMStudioDiscoverySource final : public IWindowsLMStudioDiscoverySource {
public:
    explicit WindowsLMStudioDiscoverySource(
        WindowsLMStudioDiscoverySourceOptions options = {});
    ~WindowsLMStudioDiscoverySource() override;

    WindowsLMStudioDiscoverySource(const WindowsLMStudioDiscoverySource&) = delete;
    WindowsLMStudioDiscoverySource& operator=(const WindowsLMStudioDiscoverySource&) = delete;
    WindowsLMStudioDiscoverySource(WindowsLMStudioDiscoverySource&&) = delete;
    WindowsLMStudioDiscoverySource& operator=(WindowsLMStudioDiscoverySource&&) = delete;

    [[nodiscard]] Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>
    discover(const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

struct WindowsLMStudioEnvironmentOptions final {
    static constexpr std::size_t DefaultMaximumCandidates = 64U;
    static constexpr std::size_t DefaultMaximumConfigurationBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t DefaultMaximumJsonDepth = 32U;

    std::size_t maximumCandidates{DefaultMaximumCandidates};
    std::size_t maximumConfigurationBytes{DefaultMaximumConfigurationBytes};
    std::size_t maximumJsonDepth{DefaultMaximumJsonDepth};
};

class WindowsLMStudioEnvironment final : public Contracts::ILMStudioEnvironment {
public:
    WindowsLMStudioEnvironment(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        IWindowsLMStudioDiscoverySource& discoverySource,
        WindowsLMStudioEnvironmentOptions options = {});
    ~WindowsLMStudioEnvironment() override;

    WindowsLMStudioEnvironment(const WindowsLMStudioEnvironment&) = delete;
    WindowsLMStudioEnvironment& operator=(const WindowsLMStudioEnvironment&) = delete;
    WindowsLMStudioEnvironment(WindowsLMStudioEnvironment&&) = delete;
    WindowsLMStudioEnvironment& operator=(WindowsLMStudioEnvironment&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LMStudioEnvironmentStatus> inspect(
        const std::optional<Domain::PathText>& explicitConfigurationPath,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectionHealth>
    connectionHealth(const Domain::OperationContext& context) noexcept override;

    // The role verifier/deployment composition root publishes a fully measured
    // snapshot here. This concrete seam intentionally does not widen the stable
    // ILMStudioEnvironment contract.
    [[nodiscard]] Domain::Result<void> cacheConnectionHealth(
        Domain::LMStudioConnectionHealth health,
        const Domain::OperationContext& context) noexcept;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
