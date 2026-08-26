#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "BoundaryFakeSupport.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

class DeterministicWorkspaceAuthority final
    : public Contracts::IWorkspaceAuthority {
public:
    DeterministicWorkspaceAuthority(
        Domain::AuthorityId authorityId,
        Domain::ClientId callerId,
        std::vector<Domain::PathText> trustedRoots,
        const Domain::FileAccess intent,
        std::vector<Domain::FileAccess> grants,
        std::vector<Domain::FileAccess> denials,
        const bool shellEnabled,
        const std::uint64_t generation)
        : authorityId_{std::move(authorityId)},
          callerId_{std::move(callerId)},
          trustedRoots_{std::move(trustedRoots)},
          intent_{intent},
          grants_{std::move(grants)},
          denials_{std::move(denials)},
          shellEnabled_{shellEnabled},
          generation_{generation}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    std::move(gate).error());
            }
            return issueAuthority(
                authorityId_,
                projectId,
                callerId_,
                trustedRoots_,
                intent_,
                grants_,
                denials_,
                shellEnabled_,
                generation_);
        } catch (...) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        const bool shellEnabled,
        const std::uint64_t generation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    std::move(gate).error());
            }
            return narrowAuthority(
                authority,
                std::vector<Domain::PathText>{trustedRoots},
                std::vector<Domain::FileAccess>{grants},
                shellEnabled,
                generation);
        } catch (...) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    std::move(gate).error());
            }
            const auto root = selectRoot(authority, request);
            if (!root) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The deterministic path is outside the authority roots."));
            }
            if (containsTraversal(request.requestedPath.value()) ||
                !isWithin(request.requestedPath, root.value()) ||
                (request.protectAuthorityRoot &&
                 request.requestedPath == root.value() &&
                 request.access != Domain::FileAccess::Read)) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The deterministic path request violates root protection."));
            }
            return issueAuthorizedPath(
                authority,
                request.requestedPath,
                root.value(),
                request.access);
        } catch (...) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                internalFailure());
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

private:
    [[nodiscard]] static std::optional<Domain::PathText> selectRoot(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request)
    {
        if (request.basePath) {
            const auto match = std::find(
                authority.trustedRoots().begin(),
                authority.trustedRoots().end(),
                request.basePath.value());
            if (match != authority.trustedRoots().end()) {
                return *match;
            }
            return std::nullopt;
        }

        const auto match = std::find_if(
            authority.trustedRoots().begin(),
            authority.trustedRoots().end(),
            [&](const Domain::PathText& root) {
                return isWithin(request.requestedPath, root);
            });
        if (match == authority.trustedRoots().end()) {
            return std::nullopt;
        }
        return *match;
    }

    [[nodiscard]] static bool isWithin(
        const Domain::PathText& path,
        const Domain::PathText& root) noexcept
    {
        const auto& value = path.value();
        const auto& rootValue = root.value();
        if (value == rootValue) {
            return true;
        }
        if (!value.starts_with(rootValue) || value.size() <= rootValue.size()) {
            return false;
        }
        if (rootValue.ends_with('\\') || rootValue.ends_with('/')) {
            return true;
        }
        const auto separator = value[rootValue.size()];
        return separator == '\\' || separator == '/';
    }

    [[nodiscard]] static bool containsTraversal(
        const std::string_view value) noexcept
    {
        return value == ".." ||
               value.starts_with("..\\") || value.starts_with("../") ||
               value.find("\\..\\") != std::string_view::npos ||
               value.find("/../") != std::string_view::npos ||
               value.ends_with("\\..") || value.ends_with("/..");
    }

    [[nodiscard]] static Domain::Error internalFailure()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic authority boundary could not record the operation.");
    }

    DeterministicBoundaryState state_;
    Domain::AuthorityId authorityId_;
    Domain::ClientId callerId_;
    std::vector<Domain::PathText> trustedRoots_;
    Domain::FileAccess intent_;
    std::vector<Domain::FileAccess> grants_;
    std::vector<Domain::FileAccess> denials_;
    bool shellEnabled_;
    std::uint64_t generation_;
};

} // namespace ForgeConductor::Tests::Fakes
