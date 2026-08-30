#include "ManagerStartupOwnershipProjector.h"

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

ManagerStartupOwnershipProjection ManagerStartupOwnershipProjector::project(
    std::string registrationIdentity,
    const Manager::ManagerStartupTaskOwnership& expected,
    Manager::ManagerStartupTaskOwnership observed) noexcept
{
    Manager::ManagerStartupTaskObservation observation;
    observation.exists = true;
    observation.registrationIdentity = std::move(registrationIdentity);
    observation.definition.emplace();
    observation.definition->ownership = std::move(observed);
    const bool foreign = observation.definition->ownership != expected;

    return ManagerStartupOwnershipProjection{
        std::move(observation),
        foreign};
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
