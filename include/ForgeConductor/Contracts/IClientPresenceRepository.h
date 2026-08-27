#pragma once

#include "ForgeConductor/Domain/ClientPresenceModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Contracts {

class IClientPresenceRepository {
public:
    virtual ~IClientPresenceRepository() = default;

    // Re-registering the same client key replaces its owner attributes and
    // last-seen timestamp while retaining the original first-seen timestamp.
    [[nodiscard]] virtual Domain::Result<void> upsert(
        const Domain::ClientPresenceRegistration& registration,
        const Domain::OperationContext& context) noexcept = 0;

    // False means the supplied complete identity no longer owns the row.
    [[nodiscard]] virtual Domain::Result<bool> heartbeat(
        const Domain::ClientPresenceIdentity& identity,
        Domain::UtcTimePoint observedAt,
        const Domain::OperationContext& context) noexcept = 0;

    // Removal is compare-and-delete over the complete owner identity.
    [[nodiscard]] virtual Domain::Result<bool> remove(
        const Domain::ClientPresenceIdentity& identity,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
