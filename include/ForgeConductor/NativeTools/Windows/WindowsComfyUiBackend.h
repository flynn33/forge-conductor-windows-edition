#pragma once

#include "ForgeConductor/Contracts/IComfyUiService.h"
#include <memory>

namespace ForgeConductor::NativeTools::Windows {

class WindowsComfyUiBackend final : public Contracts::IComfyUiBackend {
public:
    WindowsComfyUiBackend(Contracts::IWorkspaceAuthority& authority, Domain::PathText runtimeRoot);
    ~WindowsComfyUiBackend() override;
    WindowsComfyUiBackend(const WindowsComfyUiBackend&) = delete;
    WindowsComfyUiBackend& operator=(const WindowsComfyUiBackend&) = delete;
    [[nodiscard]] Domain::Result<std::string> perform(
        std::string_view operation, std::string_view arguments,
        const Domain::ComfyUiConfig& configuration,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    void shutdown() noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
} // namespace ForgeConductor::NativeTools::Windows
