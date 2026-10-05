#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <functional>
#include <optional>

namespace ForgeConductor::NativeTools::Windows {

class WindowsShellService final : public Contracts::IShellService {
public:
    static constexpr std::chrono::milliseconds DefaultTimeout{30'000};
    static constexpr std::chrono::milliseconds MaximumTimeout{120'000};
    static constexpr std::size_t MaximumCommandBytes = 4U * 1024U;
    static constexpr std::size_t MaximumOutputBytes = 80'000U;
    static constexpr std::size_t MaximumErrorBytes = 20'000U;
    static constexpr std::chrono::milliseconds MaximumJobTimeout{3'600'000};
    static constexpr std::size_t MaximumActiveJobs = 2U;
    static constexpr std::size_t MaximumRetainedJobs = 16U;

    using JobCompletionSink = std::function<void(const Domain::ProjectId&, const Domain::ShellJobSnapshot&)>;
    void setJobCompletionSink(JobCompletionSink sink);
    [[nodiscard]] bool supportsJobs() const noexcept override { return true; }
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> startProcess(
        const Domain::ProcessRequest&, const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> adoptJob(
        std::string_view, const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ShellJobLogPage> readJobLog(
        std::string_view, bool, std::optional<std::uint64_t>, std::size_t,
        const Contracts::WorkspaceAuthority&, const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> startJob(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> getJob(
        std::string_view jobId, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::vector<Domain::ShellJobSnapshot>> listJobs(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> cancelJob(
        std::string_view jobId, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

    WindowsShellService(
        Domain::PathText powerShellExecutable,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
        std::optional<Domain::PathText> jobRoot = std::nullopt);
    ~WindowsShellService() override;

    WindowsShellService(const WindowsShellService&) = delete;
    WindowsShellService& operator=(const WindowsShellService&) = delete;
    WindowsShellService(WindowsShellService&&) = delete;
    WindowsShellService& operator=(WindowsShellService&&) = delete;

    // The request is a command envelope: executable must exactly equal the
    // injected PowerShell path and arguments must contain one command string.
    // This service owns the fixed PowerShell switches passed to the supervisor.
    [[nodiscard]] Domain::Result<Domain::ProcessResult> execute(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    [[nodiscard]] static Domain::Result<Domain::ProcessResult> executeInternal(
        const std::shared_ptr<Impl>& implementation,
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context,
        bool managedJob,
        bool directProcess = false) noexcept;
    [[nodiscard]] Domain::Result<Domain::ShellJobSnapshot> startOwnedJob(
        const Domain::ProcessRequest&, const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&, bool directProcess) noexcept;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::NativeTools::Windows
