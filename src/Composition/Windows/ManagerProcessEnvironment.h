#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace ForgeConductor::Composition::Windows {

struct ManagerProcessEnvironmentOptions final {
    std::optional<Domain::PathText> explicitDataRoot;
    bool allowEnvironmentOverride{};

    bool operator==(const ManagerProcessEnvironmentOptions&) const = default;
};

// Stable file identity is retained with the canonical path so preparation can
// detect replacement of either process image after the read-only inspection.
struct ManagerProcessExecutableIdentity final {
    Domain::PathText canonicalPath;
    std::uint64_t volumeSerialNumber{};
    std::array<std::byte, 16U> fileIdentifier{};

    bool operator==(const ManagerProcessExecutableIdentity&) const = default;
};

struct ManagerProcessPlatformSnapshot final {
    ManagerProcessExecutableIdentity currentManagerImage;
    std::uint64_t physicalMemoryBytes{};
};

// All calls are synchronous and bounded. Implementations retain no handles
// after returning; tests can inject exact platform evidence without touching
// the real process image or filesystem.
class IManagerProcessEnvironmentPlatformProbe {
public:
    virtual ~IManagerProcessEnvironmentPlatformProbe() noexcept = default;

    [[nodiscard]] virtual Domain::Result<ManagerProcessPlatformSnapshot>
    inspect(const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<ManagerProcessExecutableIdentity>
    executableIdentity(
        const Domain::PathText& executable,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> ensureRegularDirectory(
        const Domain::PathText& directory,
        const Domain::OperationContext& context) noexcept = 0;
};

class WindowsManagerProcessEnvironmentPlatformProbe final
    : public IManagerProcessEnvironmentPlatformProbe {
public:
    static constexpr std::size_t MaximumExecutablePathCharacters =
        32U * 1024U;

    WindowsManagerProcessEnvironmentPlatformProbe() noexcept = default;
    ~WindowsManagerProcessEnvironmentPlatformProbe() noexcept override =
        default;

    WindowsManagerProcessEnvironmentPlatformProbe(
        const WindowsManagerProcessEnvironmentPlatformProbe&) = delete;
    WindowsManagerProcessEnvironmentPlatformProbe& operator=(
        const WindowsManagerProcessEnvironmentPlatformProbe&) = delete;
    WindowsManagerProcessEnvironmentPlatformProbe(
        WindowsManagerProcessEnvironmentPlatformProbe&&) = delete;
    WindowsManagerProcessEnvironmentPlatformProbe& operator=(
        WindowsManagerProcessEnvironmentPlatformProbe&&) = delete;

    [[nodiscard]] Domain::Result<ManagerProcessPlatformSnapshot> inspect(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<ManagerProcessExecutableIdentity>
    executableIdentity(
        const Domain::PathText& executable,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> ensureRegularDirectory(
        const Domain::PathText& directory,
        const Domain::OperationContext& context) noexcept override;
};

class ManagerProcessEnvironmentSnapshot final {
public:
    [[nodiscard]] const Domain::PathText& dataRoot() const noexcept
    {
        return dataRoot_;
    }

    [[nodiscard]] const Domain::PathText& configurationRoot() const noexcept
    {
        return configurationRoot_;
    }

    [[nodiscard]] const Domain::PathText& diagnosticsRoot() const noexcept
    {
        return diagnosticsRoot_;
    }

    [[nodiscard]] const Domain::PathText& exportRoot() const noexcept
    {
        return exportRoot_;
    }

    [[nodiscard]] const Domain::PathText& projectsRoot() const noexcept
    {
        return projectsRoot_;
    }

    [[nodiscard]] const Domain::PathText& managerExecutable() const noexcept
    {
        return managerExecutableIdentity_.canonicalPath;
    }

    [[nodiscard]] const Domain::PathText& cliExecutable() const noexcept
    {
        return cliExecutableIdentity_.canonicalPath;
    }

    [[nodiscard]] const ManagerProcessExecutableIdentity&
    managerExecutableIdentity() const noexcept
    {
        return managerExecutableIdentity_;
    }

    [[nodiscard]] const ManagerProcessExecutableIdentity&
    cliExecutableIdentity() const noexcept
    {
        return cliExecutableIdentity_;
    }

    [[nodiscard]] std::uint64_t physicalMemoryBytes() const noexcept
    {
        return physicalMemoryBytes_;
    }

    [[nodiscard]] Domain::ResourceProfile resourceProfile() const noexcept
    {
        return resourceProfile_;
    }

    [[nodiscard]] const Domain::ResourceBudgets& resourceBudgets() const noexcept
    {
        return resourceBudgets_;
    }

    bool operator==(const ManagerProcessEnvironmentSnapshot&) const = default;

private:
    friend class ManagerProcessEnvironment;

    ManagerProcessEnvironmentSnapshot(
        Domain::PathText dataRoot,
        Domain::PathText configurationRoot,
        Domain::PathText diagnosticsRoot,
        Domain::PathText exportRoot,
        Domain::PathText projectsRoot,
        ManagerProcessExecutableIdentity managerExecutableIdentity,
        ManagerProcessExecutableIdentity cliExecutableIdentity,
        std::uint64_t physicalMemoryBytes,
        Domain::ResourceProfile resourceProfile,
        Domain::ResourceBudgets resourceBudgets) noexcept;

    Domain::PathText dataRoot_;
    Domain::PathText configurationRoot_;
    Domain::PathText diagnosticsRoot_;
    Domain::PathText exportRoot_;
    Domain::PathText projectsRoot_;
    ManagerProcessExecutableIdentity managerExecutableIdentity_;
    ManagerProcessExecutableIdentity cliExecutableIdentity_;
    std::uint64_t physicalMemoryBytes_{};
    Domain::ResourceProfile resourceProfile_{};
    Domain::ResourceBudgets resourceBudgets_{};
};

// This is the terminal ownership object for Manager process composition. It is
// move-only because it owns the live instance lease; the snapshot remains
// externally immutable for the object's entire lifetime.
class PreparedManagerProcessEnvironment final {
public:
    ~PreparedManagerProcessEnvironment() noexcept = default;

    PreparedManagerProcessEnvironment(
        const PreparedManagerProcessEnvironment&) = delete;
    PreparedManagerProcessEnvironment& operator=(
        const PreparedManagerProcessEnvironment&) = delete;
    PreparedManagerProcessEnvironment(
        PreparedManagerProcessEnvironment&&) noexcept = default;
    PreparedManagerProcessEnvironment& operator=(
        PreparedManagerProcessEnvironment&&) noexcept = default;

    [[nodiscard]] const ManagerProcessEnvironmentSnapshot& snapshot() const
        noexcept
    {
        return snapshot_;
    }

    [[nodiscard]] const Infrastructure::Windows::WindowsManagerInstanceLease&
    lease() const noexcept
    {
        return lease_;
    }

private:
    friend class ManagerProcessEnvironment;

    PreparedManagerProcessEnvironment(
        ManagerProcessEnvironmentSnapshot snapshot,
        Infrastructure::Windows::WindowsManagerInstanceLease lease) noexcept;

    ManagerProcessEnvironmentSnapshot snapshot_;
    Infrastructure::Windows::WindowsManagerInstanceLease lease_;
};

class ManagerProcessEnvironment final {
public:
    static constexpr std::size_t RequiredDirectoryCount = 5U;

    ManagerProcessEnvironment(
        ManagerProcessEnvironmentOptions options,
        IManagerProcessEnvironmentPlatformProbe& platformProbe) noexcept;

    ManagerProcessEnvironment(const ManagerProcessEnvironment&) = delete;
    ManagerProcessEnvironment& operator=(const ManagerProcessEnvironment&) =
        delete;
    ManagerProcessEnvironment(ManagerProcessEnvironment&&) = delete;
    ManagerProcessEnvironment& operator=(ManagerProcessEnvironment&&) = delete;

    [[nodiscard]] const ManagerProcessEnvironmentOptions& options() const
        noexcept
    {
        return options_;
    }

    // Read-only. No app-owned directory is created by inspection.
    [[nodiscard]] Domain::Result<ManagerProcessEnvironmentSnapshot> inspect(
        const Domain::OperationContext& context) const noexcept;

    // The caller must acquire the instance lease first and transfer it here.
    // Every root and executable identity is re-resolved before the first
    // directory mutation. Success retains the lease beside that exact snapshot.
    [[nodiscard]] Domain::Result<PreparedManagerProcessEnvironment>
    prepareAfterLease(
        const ManagerProcessEnvironmentSnapshot& inspectedSnapshot,
        Infrastructure::Windows::WindowsManagerInstanceLease lease,
        const Domain::OperationContext& context) const noexcept;

private:
    const ManagerProcessEnvironmentOptions options_;
    IManagerProcessEnvironmentPlatformProbe& platformProbe_;
};

} // namespace ForgeConductor::Composition::Windows
