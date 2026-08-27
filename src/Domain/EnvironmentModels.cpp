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

std::string_view wireName(const LMStudioDiscoverySource source) noexcept
{
    switch (source) {
    case LMStudioDiscoverySource::ExplicitConfiguration:
        return "explicit_configuration";
    case LMStudioDiscoverySource::InstalledApplication:
        return "installed_application";
    case LMStudioDiscoverySource::KnownUserLocation:
        return "known_user_location";
    case LMStudioDiscoverySource::RunningProcess:
        return "running_process";
    }
    return "known_user_location";
}

} // namespace ForgeConductor::Domain
