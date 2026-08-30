#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Domain {

struct ClientPresenceLimits final {
    static constexpr std::size_t MaximumRoleBytes = 64U;
};

// The complete owner identity is retained at the lifecycle boundary so a
// superseded process cannot heartbeat or remove a replacement that reused the
// same durable client key.
struct ClientPresenceIdentity final {
    ClientId clientId;
    std::string role;
    std::optional<DeploymentId> deploymentId;
    std::optional<std::uint32_t> processId;

    bool operator==(const ClientPresenceIdentity&) const = default;
};

struct ClientPresenceRegistration final {
    ClientPresenceIdentity identity;
    PathText workingDirectory;
    UtcTimePoint firstSeenAt;
    UtcTimePoint lastSeenAt;

    bool operator==(const ClientPresenceRegistration&) const = default;
};

[[nodiscard]] Result<void> validateClientPresenceIdentity(
    const ClientPresenceIdentity& identity) noexcept;

[[nodiscard]] Result<void> validateClientPresenceRegistration(
    const ClientPresenceRegistration& registration) noexcept;

} // namespace ForgeConductor::Domain
