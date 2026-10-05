#pragma once
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include <functional>
#include <string>
#include <vector>

namespace ForgeConductor::Contracts {
struct EvidenceStoragePaths final {
    AuthorizedPath read;
    AuthorizedPath write;
    AuthorizedPath create;
};
using EvidenceStorageResolver = std::function<Domain::Result<EvidenceStoragePaths>(
    const Domain::ProjectId&, const Domain::OperationContext&)>;
class IEvidenceService {
public:
    virtual ~IEvidenceService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> digest(
        const std::vector<Domain::PathText>& paths, const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<std::string> readLog(
        const WorkspaceAuthority& authority, std::size_t offset, std::size_t limit,
        bool verify, const Domain::OperationContext& context) noexcept = 0;
};
} // namespace ForgeConductor::Contracts
