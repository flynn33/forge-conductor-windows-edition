#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

class IComfyUiService {
public:
    virtual ~IComfyUiService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        std::string_view name, std::string_view arguments,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

// Native provider operations are injectable separately from durable job state.
// Only the service submits generation; workflow/front-end operations never queue.
class IComfyUiBackend {
public:
    virtual ~IComfyUiBackend() = default;
    [[nodiscard]] virtual Domain::Result<std::string> perform(
        std::string_view operation, std::string_view arguments,
        const Domain::ComfyUiConfig& configuration,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
