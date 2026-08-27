#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {

class WindowsGitService final : public Contracts::IGitService {
public:
    static constexpr std::size_t MaximumOutputBytes = 80'000U;
    static constexpr std::size_t MaximumErrorBytes = 20'000U;
    static constexpr std::size_t MaximumLogEntries = 200U;
    static constexpr std::size_t MaximumAddPaths = 200U;
    static constexpr std::size_t MaximumArgumentBytes = 4U * 1024U;

    WindowsGitService(
        Domain::PathText gitExecutable,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor);
    ~WindowsGitService() override = default;

    WindowsGitService(const WindowsGitService&) = delete;
    WindowsGitService& operator=(const WindowsGitService&) = delete;
    WindowsGitService(WindowsGitService&&) = delete;
    WindowsGitService& operator=(WindowsGitService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> status(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> diff(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        std::span<const std::string> arguments,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> log(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        std::size_t maximumEntries,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> add(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        std::span<const Contracts::AuthorizedPath> paths,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> commit(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        std::string_view message,
        const Domain::OperationContext& context) noexcept override;

private:
    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Contracts::AuthorizedPath& repository,
        const Contracts::WorkspaceAuthority& authority,
        Domain::FileAccess repositoryAccess,
        std::vector<std::string> arguments,
        std::size_t maximumStdoutBytes,
        const Domain::OperationContext& context) noexcept;

    const Domain::PathText gitExecutable_;
    const std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor_;
};

} // namespace ForgeConductor::NativeTools::Windows
