#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

class IArtifactDocumentService {
public:
    virtual ~IArtifactDocumentService() = default;

    // Arguments and successful receipts are JSON objects. Implementations own
    // validation, path authorization, package creation and atomic publication.
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        std::string_view name, std::string_view arguments,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
