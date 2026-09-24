#pragma once

#include "ForgeConductor/Contracts/IProjectPolicyService.h"

namespace ForgeConductor::Infrastructure::Windows {

// Retrieves text as data. Never executes repository code or downloads an
// unpinned branch after resolving the immutable source commit.
class WindowsPolicySourceReader final : public Contracts::IPolicySourceReader {
public:
    Domain::Result<Contracts::PolicySourceBundle> read(
        const std::string&, const Domain::OperationContext&) noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
