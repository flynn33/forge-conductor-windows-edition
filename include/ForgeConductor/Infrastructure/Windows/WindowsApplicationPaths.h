#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"

#include <optional>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsApplicationPathsOptions final {
    std::optional<Domain::PathText> explicitDataRoot;
    bool allowEnvironmentOverride{};
};

class WindowsApplicationPaths final : public Contracts::IApplicationPaths {
public:
    explicit WindowsApplicationPaths(
        WindowsApplicationPathsOptions options = {}) noexcept;

    WindowsApplicationPaths(const WindowsApplicationPaths&) = delete;
    WindowsApplicationPaths& operator=(const WindowsApplicationPaths&) = delete;
    WindowsApplicationPaths(WindowsApplicationPaths&&) = delete;
    WindowsApplicationPaths& operator=(WindowsApplicationPaths&&) = delete;

    [[nodiscard]] Domain::Result<Domain::PathText> dataRoot(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::PathText> configurationRoot(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::PathText> diagnosticsRoot(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::PathText> projectRoot(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

private:
    [[nodiscard]] Domain::Result<Domain::PathText> rootFor(
        const Domain::OperationContext& context,
        const char* action) const noexcept;
    [[nodiscard]] Domain::Result<Domain::PathText> childFor(
        const Domain::OperationContext& context,
        const wchar_t* relativePath,
        const char* action) const noexcept;

    const Domain::Result<Domain::PathText> dataRoot_;
};

} // namespace ForgeConductor::Infrastructure::Windows
