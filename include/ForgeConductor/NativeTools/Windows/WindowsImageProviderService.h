#pragma once

#include "ForgeConductor/Contracts/IImageProviderService.h"
#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <memory>

namespace ForgeConductor::NativeTools::Windows {
class WindowsImageProviderService final : public Contracts::IImageProviderService {
public:
    WindowsImageProviderService(Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IAtomicFileStore& files, Contracts::IConfigurationStore& configuration,
        Contracts::IWorkspaceAuthority& storageAuthority, const Contracts::WorkspaceAuthority& storageScope,
        Domain::PathText jobsRoot, Contracts::IUuidGenerator& uuidGenerator,
        Contracts::IClock& clock, Contracts::IHasher& hasher,
        std::shared_ptr<Contracts::IImageProviderHttpTransport> transport = {});
    ~WindowsImageProviderService() noexcept override;
    [[nodiscard]] Domain::Result<std::string> execute(std::string_view name,
        std::string_view arguments, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    void shutdown() noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
} // namespace ForgeConductor::NativeTools::Windows
