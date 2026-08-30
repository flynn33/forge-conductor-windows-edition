#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioCandidateSelector.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Composition::Windows {

struct ManagerLmStudioReadScopeIdentity final {
    Domain::AuthorityId authorityId;
    Domain::ProjectId projectId;
    Domain::ClientId callerId;
    std::uint64_t generation{1U};

    bool operator==(const ManagerLmStudioReadScopeIdentity&) const = default;
};

struct ManagerLmStudioReadScopeConfiguration final {
    // The selector's validation capability and the final read capability have
    // separate injected identities. Reusing one authorityId would make broad
    // and narrow capabilities indistinguishable to value-bound consumers.
    ManagerLmStudioReadScopeIdentity selectionIdentity;
    ManagerLmStudioReadScopeIdentity readScopeIdentity;
    Domain::PathText forgeDataRoot;
    Domain::PathText forgeCliExecutable;

    bool operator==(
        const ManagerLmStudioReadScopeConfiguration&) const = default;
};

// Move-only lifetime aggregate. The concrete issuer remains alive beside the
// immutable capability it issued, and neither object can be replaced after
// successful resolution.
class ManagerLmStudioReadScope final {
public:
    ManagerLmStudioReadScope(const ManagerLmStudioReadScope&) = delete;
    ManagerLmStudioReadScope& operator=(
        const ManagerLmStudioReadScope&) = delete;
    ManagerLmStudioReadScope(ManagerLmStudioReadScope&&) noexcept = default;
    ManagerLmStudioReadScope& operator=(ManagerLmStudioReadScope&&) = delete;

    [[nodiscard]] Infrastructure::Windows::WindowsWorkspaceAuthority& issuer()
        noexcept
    {
        return *issuer_;
    }

    [[nodiscard]] const Contracts::WorkspaceAuthority& authority() const
        noexcept
    {
        return authority_;
    }

private:
    friend class ManagerLmStudioReadScopeResolver;

    ManagerLmStudioReadScope(
        std::unique_ptr<
            Infrastructure::Windows::WindowsWorkspaceAuthority> issuer,
        Contracts::WorkspaceAuthority authority) noexcept;

    std::unique_ptr<
        Infrastructure::Windows::WindowsWorkspaceAuthority> issuer_;
    const Contracts::WorkspaceAuthority authority_;
};

// Converts one selector-produced, authority-bound snapshot into the narrow
// read capability used by Manager Doctor and periodic LM Studio maintenance.
class ManagerLmStudioReadScopeResolver final {
public:
    static constexpr std::size_t MaximumCandidateEvaluations = 64U;
    static constexpr std::size_t MaximumAuthorityRoots = 4U;

    explicit ManagerLmStudioReadScopeResolver(
        ManagerLmStudioReadScopeConfiguration configuration) noexcept;

    ManagerLmStudioReadScopeResolver(
        const ManagerLmStudioReadScopeResolver&) = delete;
    ManagerLmStudioReadScopeResolver& operator=(
        const ManagerLmStudioReadScopeResolver&) = delete;
    ManagerLmStudioReadScopeResolver(
        ManagerLmStudioReadScopeResolver&&) = delete;
    ManagerLmStudioReadScopeResolver& operator=(
        ManagerLmStudioReadScopeResolver&&) = delete;

    [[nodiscard]] const ManagerLmStudioReadScopeConfiguration& configuration()
        const noexcept
    {
        return configuration_;
    }

    [[nodiscard]] Domain::Result<ManagerLmStudioReadScope> resolve(
        const Infrastructure::Windows::WindowsLMStudioCandidateSelection&
            selection,
        const Domain::OperationContext& context) const noexcept;

private:
    const ManagerLmStudioReadScopeConfiguration configuration_;
};

} // namespace ForgeConductor::Composition::Windows
