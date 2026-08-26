#pragma once

#include "ForgeConductor/Contracts/AuthorizedToolCall.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <cstddef>
#include <span>

namespace ForgeConductor::Contracts {

// Publishes and retains project-memory artifacts only beneath the selected
// project's app-owned exports directory. Implementations must reject reparses,
// hard links, alternate streams, path escapes, and non-regular files.
class IProjectMemoryArtifactStore {
public:
    // Exported snapshots, quarantine entries, and abandoned staging files
    // share this per-project hard quota. Implementations must count through
    // retained directory handles and reject publication at the quota. Moving
    // an arbitrarily named import into an owned quarantine name must also be
    // rejected when that transition would exceed the quota.
    static constexpr std::size_t MaximumOwnedArtifactFilesPerProject = 256U;

    virtual ~IProjectMemoryArtifactStore() = default;

    [[nodiscard]] virtual Domain::Result<Domain::PathText> publish(
        const Domain::ProjectId& projectId,
        std::span<const std::byte> content,
        const WorkspaceAuthority& writeAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ProjectMemoryArtifactDocument> read(
        const Domain::ProjectId& projectId,
        const Domain::PathText& artifact,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;

    // Consumes the same open immediate-child handle on which the preceding
    // read() detected an oversized artifact, revalidates its size, and moves
    // that handle atomically. Implementations must bound retained handles and
    // must not reopen the source path. Preview imports never call this mutation
    // boundary and therefore remain non-mutating.
    [[nodiscard]] virtual Domain::Result<Domain::PathText> quarantineOversized(
        const Domain::ProjectId& projectId,
        const Domain::PathText& artifact,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;

    // Quarantines an artifact only after read() successfully retained and
    // returned the exact document. Implementations must revalidate and compare
    // the retained bytes before moving the immediate export child. Preview
    // imports must never call this mutation boundary.
    [[nodiscard]] virtual Domain::Result<Domain::PathText> quarantineCorrupt(
        const Domain::ProjectId& projectId,
        const Domain::ProjectMemoryArtifactDocument& retainedDocument,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
