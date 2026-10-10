#pragma once

#include "ForgeConductor/Contracts/IComfyUiService.h"
#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include <functional>
#include <memory>

namespace ForgeConductor::NativeTools::Windows {

class WindowsComfyUiService final : public Contracts::IComfyUiService {
public:
    WindowsComfyUiService(Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IAtomicFileStore& files, Contracts::IConfigurationStore& configuration,
        Contracts::IWorkspaceAuthority& storageAuthority,
        const Contracts::WorkspaceAuthority& storageScope, Domain::PathText jobsRoot,
        Contracts::IUuidGenerator& uuidGenerator, Contracts::IClock& clock,
        Contracts::IHasher& hasher, std::shared_ptr<Contracts::IComfyUiBackend> backend = {});
    ~WindowsComfyUiService() noexcept override;
    [[nodiscard]] Domain::Result<std::string> execute(std::string_view name,
        std::string_view arguments, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    // Saved native conversation evidence; populated by Manager, never by MCP args.
    void setConversationObserver(std::function<Domain::Result<std::string>(
        const Domain::ProjectId&, std::string_view boundConversation,
        const Domain::OperationContext&)> observer);
    void shutdown() noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::NativeTools::Windows
