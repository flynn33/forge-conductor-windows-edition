#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <vector>

namespace ForgeConductor::Domain {

struct ForgeStatusLimits final {
    static constexpr std::size_t MaximumOpenSessionIds =
        AgentSessionLimits::MaximumSessionQueryRows;
};

// Bounded durable state used by the MCP forge_status projection. Runtime
// identity, catalogs, memory, and continuity remain owned by their existing
// services and are deliberately not duplicated here.
struct ForgeStatusProjection final {
    std::size_t presenceCount{};
    std::vector<SessionId> openSessionIds;

    bool operator==(const ForgeStatusProjection&) const = default;
};

[[nodiscard]] Result<void> validateForgeStatusProjection(
    const ForgeStatusProjection& projection) noexcept;

} // namespace ForgeConductor::Domain
