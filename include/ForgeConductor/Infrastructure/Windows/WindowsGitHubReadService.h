#pragma once

#include "ForgeConductor/Contracts/IGitHubReadService.h"

#include <optional>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsGitHubReadService final : public Contracts::IGitHubReadService {
public:
    explicit WindowsGitHubReadService(std::optional<std::string> bearerToken = std::nullopt);
    [[nodiscard]] Domain::Result<std::string> read(
        const Contracts::GitHubReadRequest& request,
        const Domain::OperationContext& context) noexcept override;
    // Only tokens explicitly configured in this process environment are read.
    [[nodiscard]] static std::optional<std::string> configuredEnvironmentToken() noexcept;
    [[nodiscard]] static Domain::Result<std::string> projectResponse(
        const Contracts::GitHubReadRequest& request, std::string_view body,
        bool authenticated, bool hasNextPage) noexcept;
    [[nodiscard]] static Domain::Result<std::string> requestPath(
        const Contracts::GitHubReadRequest& request) noexcept;

private:
    const std::optional<std::string> bearerToken_;
};

} // namespace ForgeConductor::Infrastructure::Windows
