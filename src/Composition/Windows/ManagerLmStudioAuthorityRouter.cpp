#include "ManagerLmStudioAuthorityRouter.h"

#include "ForgeConductor/Domain/Error.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace ForgeConductor::Composition::Windows {
namespace {

[[nodiscard]] bool hasExactReadAccess(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto& grants = authority.grants();
    const auto& denials = authority.denials();
    return authority.intent() == Domain::FileAccess::Read &&
        grants.size() == 1U && grants[0] == Domain::FileAccess::Read &&
        denials.size() == 4U &&
        denials[0] == Domain::FileAccess::Write &&
        denials[1] == Domain::FileAccess::Create &&
        denials[2] == Domain::FileAccess::Delete &&
        denials[3] == Domain::FileAccess::Execute &&
        !authority.shellEnabled();
}

[[nodiscard]] bool hasExactDeploymentAccess(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto& grants = authority.grants();
    return authority.intent() == Domain::FileAccess::Write &&
        grants.size() == 5U &&
        grants[0] == Domain::FileAccess::Read &&
        grants[1] == Domain::FileAccess::Write &&
        grants[2] == Domain::FileAccess::Create &&
        grants[3] == Domain::FileAccess::Delete &&
        grants[4] == Domain::FileAccess::Execute &&
        authority.denials().empty() && authority.shellEnabled();
}

[[nodiscard]] Domain::Error foreignAuthorityError()
{
    return Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "The LM Studio capability identifier is not owned by the Manager "
        "authority router.");
}

[[nodiscard]] Domain::Error internalError()
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The Manager LM Studio authority router failed safely.");
}

} // namespace

ManagerLmStudioAuthorityRouter::ManagerLmStudioAuthorityRouter(
    Infrastructure::Windows::WindowsWorkspaceAuthority& readIssuer,
    Contracts::WorkspaceAuthority readAuthority,
    Infrastructure::Windows::WindowsWorkspaceAuthority& writeIssuer,
    Contracts::WorkspaceAuthority writeAuthority)
    : readIssuer_{readIssuer},
      writeIssuer_{writeIssuer},
      readAuthority_{std::move(readAuthority)},
      writeAuthority_{std::move(writeAuthority)}
{
    if (&readIssuer_ == &writeIssuer_ ||
        readAuthority_.authorityId() == writeAuthority_.authorityId() ||
        readAuthority_.projectId() != writeAuthority_.projectId() ||
        readAuthority_.callerId() != writeAuthority_.callerId() ||
        readAuthority_.generation() == 0U ||
        writeAuthority_.generation() == 0U ||
        !hasExactReadAccess(readAuthority_) ||
        !hasExactDeploymentAccess(writeAuthority_)) {
        throw std::invalid_argument{
            "ManagerLmStudioAuthorityRouter requires distinct issuers and "
            "canonical read/deployment capabilities for one project and caller."};
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
ManagerLmStudioAuthorityRouter::authorityFor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto read = readIssuer_.authorityFor(projectId, context);
        if (!read) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(read).error());
        }
        auto write = writeIssuer_.authorityFor(projectId, context);
        if (!write) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(write).error());
        }
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The Manager LM Studio project has distinct read and deployment "
                "capabilities; aggregate authority issuance is ambiguous."));
    } catch (...) {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            internalError());
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
ManagerLmStudioAuthorityRouter::narrow(
    const Contracts::WorkspaceAuthority& authority,
    const std::vector<Domain::PathText>& trustedRoots,
    const std::vector<Domain::FileAccess>& grants,
    const bool shellEnabled,
    const std::uint64_t generation,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (authority.authorityId() == readAuthority_.authorityId()) {
            return readIssuer_.narrow(
                authority, trustedRoots, grants, shellEnabled, generation,
                context);
        }
        if (authority.authorityId() == writeAuthority_.authorityId()) {
            return writeIssuer_.narrow(
                authority, trustedRoots, grants, shellEnabled, generation,
                context);
        }
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            foreignAuthorityError());
    } catch (...) {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            internalError());
    }
}

Domain::Result<Contracts::AuthorizedPath>
ManagerLmStudioAuthorityRouter::authorize(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathAuthorizationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (authority.authorityId() == readAuthority_.authorityId()) {
            return readIssuer_.authorize(authority, request, context);
        }
        if (authority.authorityId() == writeAuthority_.authorityId()) {
            return writeIssuer_.authorize(authority, request, context);
        }
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            foreignAuthorityError());
    } catch (...) {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            internalError());
    }
}

} // namespace ForgeConductor::Composition::Windows
