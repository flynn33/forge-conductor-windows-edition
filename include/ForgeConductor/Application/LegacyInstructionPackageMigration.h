#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <optional>

namespace ForgeConductor::Application {

// Call only when there are no queue rows and no saved queue order. A saved
// empty order records an intentional removal and must suppress legacy import.
[[nodiscard]] Domain::Result<std::optional<Domain::ProjectMemoryRecord>>
migrateLegacyInstructionPackage(
    Contracts::IProjectMemoryService& memory,
    Contracts::IHasher& hasher,
    Contracts::IClock& clock,
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept;

} // namespace ForgeConductor::Application
