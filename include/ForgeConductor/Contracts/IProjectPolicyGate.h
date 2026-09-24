#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/ToolModels.h"

namespace ForgeConductor::Contracts {

// Policy is checked after workspace identity validation and before issuing a
// capability. An imported instruction or model assertion cannot satisfy this
// boundary: the implementation reads the separately adopted project policy.
class IProjectPolicyGate {
public:
    virtual ~IProjectPolicyGate() = default;
    [[nodiscard]] virtual Domain::Result<void> check(
        const Domain::ToolAuthorizationRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
