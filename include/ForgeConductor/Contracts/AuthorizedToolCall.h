#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ForgeConductor::Contracts {

class IToolAuthorizer;

class AuthorizedToolCall final {
public:
    AuthorizedToolCall(const AuthorizedToolCall&) = default;
    AuthorizedToolCall(AuthorizedToolCall&&) = default;
    AuthorizedToolCall& operator=(const AuthorizedToolCall&) = delete;
    AuthorizedToolCall& operator=(AuthorizedToolCall&&) = delete;

    [[nodiscard]] const Domain::ToolCallRequest& request() const noexcept
    {
        return request_;
    }

    [[nodiscard]] const Domain::RequestId& requestId() const noexcept
    {
        return request_.metadata.requestId;
    }

    [[nodiscard]] const Domain::CorrelationId& correlationId() const noexcept
    {
        return request_.metadata.correlationId;
    }

    [[nodiscard]] const Domain::ClientId& clientId() const noexcept
    {
        return request_.metadata.clientId;
    }

    [[nodiscard]] const std::string& toolName() const noexcept
    {
        return request_.toolName;
    }

    [[nodiscard]] const std::string& canonicalRequest() const noexcept
    {
        return request_.canonicalArguments;
    }

    [[nodiscard]] Domain::ToolEffect effect() const noexcept { return effect_; }

    [[nodiscard]] const std::optional<Domain::ProjectId>& projectId() const noexcept
    {
        return request_.metadata.projectId;
    }

    [[nodiscard]] const Domain::AuthorityId& authorityId() const noexcept
    {
        return authorityId_;
    }

    [[nodiscard]] std::uint64_t authorityGeneration() const noexcept
    {
        return authorityGeneration_;
    }

    [[nodiscard]] bool matches(
        const Domain::ToolCallRequest& request) const noexcept
    {
        return request_.metadata.requestId == request.metadata.requestId &&
            request_.metadata.correlationId == request.metadata.correlationId &&
            request_.metadata.clientId == request.metadata.clientId &&
            request_.metadata.projectId == request.metadata.projectId &&
            request_.metadata.protocolVersion == request.metadata.protocolVersion &&
            request_.toolName == request.toolName &&
            request_.canonicalArguments == request.canonicalArguments;
    }

    [[nodiscard]] bool matches(
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) const noexcept
    {
        return authorityId_ == authority.authorityId() &&
            authorityGeneration_ == authority.generation() &&
            request_.metadata.clientId == authority.callerId() &&
            request_.metadata.correlationId == context.correlationId &&
            (!request_.metadata.projectId ||
             request_.metadata.projectId.value() == authority.projectId());
    }

    [[nodiscard]] bool matchesProject(
        const Domain::ProjectId& projectId) const noexcept
    {
        return request_.metadata.projectId &&
            request_.metadata.projectId.value() == projectId;
    }

private:
    friend class IToolAuthorizer;

    AuthorizedToolCall(
        Domain::ToolCallRequest request,
        const Domain::ToolEffect effect,
        Domain::AuthorityId authorityId,
        const std::uint64_t authorityGeneration)
        : request_{std::move(request)},
          effect_{effect},
          authorityId_{std::move(authorityId)},
          authorityGeneration_{authorityGeneration}
    {
    }

    const Domain::ToolCallRequest request_;
    const Domain::ToolEffect effect_;
    const Domain::AuthorityId authorityId_;
    const std::uint64_t authorityGeneration_;
};

} // namespace ForgeConductor::Contracts
