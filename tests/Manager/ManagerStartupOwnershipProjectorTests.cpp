#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"
#include "Infrastructure/Windows/Detail/ManagerStartupOwnershipProjector.h"
#include "Infrastructure/TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;

static_assert(std::is_final_v<Detail::ManagerStartupOwnershipProjection>);
static_assert(std::is_final_v<Detail::ManagerStartupOwnershipProjector>);

[[nodiscard]] Domain::ManagerStartupDefinition startupDefinition()
{
    return Domain::ManagerStartupDefinition{
        take(Domain::PathText::create(
            "C:\\Forge\\ForgeConductor.Manager.exe")),
        take(Domain::PathText::create("C:\\ForgeHome"))};
}

[[nodiscard]] Detail::ManagerStartupResolvedRegistration registration()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    return take(Detail::ManagerStartupDefinitionBuilder::build(
        startupDefinition(), identity, "ownership-projection"));
}

void requireOwnershipOnly(
    const Manager::ManagerStartupTaskObservation& observation,
    const std::string& registrationIdentity,
    const Manager::ManagerStartupTaskOwnership& ownership)
{
    require(
        observation.exists && !observation.launchProjectionComplete &&
            observation.registrationIdentity.has_value() &&
            *observation.registrationIdentity == registrationIdentity &&
            observation.definition.has_value(),
        "the ownership projection did not return a coherent partial existing observation");
    require(
        observation.definition->ownership == ownership,
        "the ownership projection did not preserve the bounded native ownership values");
    require(
        observation.definition->principal.id.empty() &&
            observation.definition->principal.userIdentity.empty() &&
            observation.definition->principal.logonType ==
                Manager::ManagerStartupTaskLogonType::Other &&
            observation.definition->triggers.empty() &&
            observation.definition->actionContext.empty() &&
            observation.definition->actions.empty() &&
            observation.definition->settings.compatibility ==
                Manager::ManagerStartupTaskCompatibility::Other &&
            observation.definition->settings.priority == 0U &&
            !observation.definition->settings.useUnifiedSchedulingEngine,
        "the foreign observation synthesized unsupported launch fields");
    require(
        !observation.enabled && !observation.running &&
            !observation.lastResult.has_value() &&
            !observation.lastRunAt.has_value(),
        "the foreign observation synthesized state or execution history");
}

void testForeignOwnershipBypassesLaunchProjection()
{
    const auto resolved = registration();
    std::vector<Manager::ManagerStartupTaskOwnership> foreignOwnership{
        resolved.definition.ownership,
        resolved.definition.ownership};
    foreignOwnership[0].source = "Foreign.Manager.Startup";
    foreignOwnership[1].uri = "\\Foreign.Manager.Startup";
    const std::string registrationIdentity =
        resolved.definition.ownership.uri;

    for (auto ownership : foreignOwnership) {
        auto projection = Detail::ManagerStartupOwnershipProjector::project(
            registrationIdentity,
            resolved.definition.ownership,
            ownership);
        require(
            projection.foreign,
            "a bounded foreign Source or URI was not recognized before launch projection");
        requireOwnershipOnly(
            projection.observation, registrationIdentity, ownership);

        const auto classified = take(
            Manager::ManagerStartupTaskPolicy::classify(
                resolved.definition, projection.observation));
        require(
            classified.state == Domain::ManagerStartupState::ForeignConflict &&
                classified.registered && !classified.enabled &&
                !classified.definitionMatches && !classified.running &&
                !classified.lastResult.has_value() &&
                !classified.lastRunAt.has_value(),
            "an ownership-only foreign observation was not classified as ForeignConflict");
    }
}

void testOwnedProjectionRequiresRemainingFields()
{
    const auto resolved = registration();
    auto projection = Detail::ManagerStartupOwnershipProjector::project(
        resolved.definition.ownership.uri,
        resolved.definition.ownership,
        resolved.definition.ownership);
    require(
        !projection.foreign,
        "matching ownership incorrectly bypassed the remaining launch projection");
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            resolved.definition, projection.observation),
        Domain::ErrorCodes::IntegrityFailure,
        "an incomplete owned projection was accepted as canonical");
}

} // namespace

void registerManagerStartupOwnershipProjectorTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup_ownership_projection_foreign_short_circuit",
        testForeignOwnershipBypassesLaunchProjection);
    addTest(
        tests,
        "manager_startup_ownership_projection_owned_requires_full_projection",
        testOwnedProjectionRequiresRemainingFields);
}

} // namespace ForgeConductor::Tests
