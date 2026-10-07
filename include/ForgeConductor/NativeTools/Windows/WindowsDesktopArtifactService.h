#pragma once

#include "ForgeConductor/Contracts/IDesktopArtifactService.h"

namespace ForgeConductor::NativeTools::Windows {

class WindowsDesktopArtifactService final : public Contracts::IDesktopArtifactService {
public:
    WindowsDesktopArtifactService(Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IAtomicFileStore& files) noexcept;
    [[nodiscard]] Domain::Result<std::string> execute(
        std::string_view toolName, std::string_view arguments,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
private:
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    Contracts::IAtomicFileStore& files_;
};

} // namespace ForgeConductor::NativeTools::Windows
