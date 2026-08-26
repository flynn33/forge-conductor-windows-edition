#include "ForgeConductor/Domain/EnvironmentModels.h"

namespace ForgeConductor::Domain {

LMStudioConnectionState deriveLMStudioConnectionState(
    const std::vector<LMStudioConnectorHealth>& roles) noexcept
{
    bool primary{};
    bool fallback{};
    for (const auto& role : roles) {
        if (role.role == LMStudioConnectorRole::Primary) primary = primary || role.ready;
        if (role.role == LMStudioConnectorRole::Fallback) fallback = fallback || role.ready;
    }
    if (primary && fallback) return LMStudioConnectionState::Ready;
    if (primary) return LMStudioConnectionState::PrimaryOnly;
    if (fallback) return LMStudioConnectionState::FallbackPromoted;
    return LMStudioConnectionState::Unavailable;
}

} // namespace ForgeConductor::Domain
