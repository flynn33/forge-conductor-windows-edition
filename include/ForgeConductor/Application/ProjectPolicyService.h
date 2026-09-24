#pragma once

#include "ForgeConductor/Contracts/IProjectPolicyService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include <functional>
#include <memory>

namespace ForgeConductor::Application {

struct PolicyStoragePaths final {
    Contracts::AuthorizedPath read;
    Contracts::AuthorizedPath write;
    Contracts::AuthorizedPath create;
};

// The Manager is the sole writer. MCP compositions use the same persisted
// snapshots through check(), never through the user-facing adoption commands.
class ProjectPolicyService final : public Contracts::IProjectPolicyService {
public:
    using Paths = std::function<Domain::Result<PolicyStoragePaths>(const Domain::ProjectId&, const Domain::OperationContext&)>;
    ProjectPolicyService(Contracts::IPolicySourceReader&, Contracts::IAtomicFileStore&,
        Contracts::IHasher&, Contracts::IProjectRegistryRepository&, Paths, std::string protectedRoot = {});
    ~ProjectPolicyService() override;
    Domain::Result<std::string> execute(const Contracts::ProjectPolicyRequest&,
        const Domain::OperationContext&) noexcept override;
    Domain::Result<void> check(const Domain::ToolAuthorizationRequest&,
        const Contracts::WorkspaceAuthority&, const Domain::OperationContext&) noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
