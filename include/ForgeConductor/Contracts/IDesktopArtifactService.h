#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include <cstddef>
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

class IDesktopArtifactService {
public:
    static constexpr std::size_t MaximumArgumentBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumImageBytes = 16U * 1024U * 1024U;
    virtual ~IDesktopArtifactService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        std::string_view toolName, std::string_view arguments,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
