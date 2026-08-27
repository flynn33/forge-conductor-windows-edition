#pragma once

#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

inline constexpr std::size_t MaximumDeterministicToolDescriptors = 64;

class DeterministicToolAuthorizerFake final
    : public Contracts::IToolAuthorizer {
public:
    DeterministicToolAuthorizerFake(
        std::string expectedToolName,
        const Domain::ToolEffect expectedEffect,
        const Domain::MonotonicTimePoint now = {})
        : expectedToolName_{std::move(expectedToolName)},
          expectedEffect_{expectedEffect},
          now_{now}
    {
        if (expectedToolName_.empty()) {
            throw std::invalid_argument{
                "The deterministic authorizer requires a tool name."};
        }
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedToolCall> authorize(
        const Domain::ToolAuthorizationRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastRequest_ = request;
            lastContext_ = context;
            if (shutdown_ || context.isCancellationRequested() ||
                (cancelledOperation_ &&
                 cancelledOperation_.value() == context.operationId)) {
                return failure(Domain::ErrorCodes::Cancelled,
                    "The deterministic tool authorization was cancelled.");
            }
            if (context.isExpired(now_)) {
                return failure(Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic tool authorization deadline expired.");
            }
            if (request.call.toolName != expectedToolName_ ||
                request.effect != expectedEffect_) {
                return failure(Domain::ErrorCodes::Unauthorized,
                    "The requested tool or effect is not authorized.");
            }
            return issueAuthorizedToolCall(request, authority, context);
        } catch (...) {
            return failure(Domain::ErrorCodes::InternalFailure,
                "The deterministic tool authorization could not be recorded.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            cancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void shutdown() noexcept { shutdown_ = true; }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::ToolAuthorizationRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return lastContext_;
    }

private:
    [[nodiscard]] static Domain::Result<Contracts::AuthorizedToolCall> failure(
        const std::string_view code,
        const char* const message)
    {
        return Domain::Result<Contracts::AuthorizedToolCall>::failure(
            Domain::makeError(code, message));
    }

    std::string expectedToolName_;
    Domain::ToolEffect expectedEffect_;
    Domain::MonotonicTimePoint now_{};
    std::optional<Domain::ToolAuthorizationRequest> lastRequest_;
    std::optional<Domain::OperationContext> lastContext_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::size_t calls_{};
    bool shutdown_{};
};

class RecordingToolHandlerFake final : public Contracts::IToolHandler {
public:
    explicit RecordingToolHandlerFake(
        std::vector<Domain::McpToolDescriptor> descriptors,
        const Domain::MonotonicTimePoint now = {})
        : descriptors_{std::move(descriptors)},
          now_{now}
    {
        if (descriptors_.empty() ||
            descriptors_.size() > MaximumDeterministicToolDescriptors) {
            throw std::invalid_argument{
                "The deterministic handler descriptor count is invalid."};
        }
    }

    DeterministicResult<Domain::ToolCallOutcome> handleResult;

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override
    {
        return descriptors_;
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> handle(
        const Contracts::AuthorizedToolCall& authorizedCall,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastContext_ = context;
            if (context.isCancellationRequested()) {
                return failure(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic tool handler was cancelled.");
            }
            if (context.isExpired(now_)) {
                return failure(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic tool handler deadline expired.");
            }
            if (authorizedCall.correlationId() != context.correlationId) {
                return failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The authorized tool correlation does not match the operation.");
            }
            if (!authorizedCall.matches(authority, context)) {
                return failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The authorized tool does not match workspace authority.");
            }
            const auto descriptor = std::find_if(
                descriptors_.begin(),
                descriptors_.end(),
                [&](const Domain::McpToolDescriptor& candidate) {
                    return candidate.tool.name == authorizedCall.toolName();
                });
            if (descriptor == descriptors_.end() ||
                descriptor->tool.effect != authorizedCall.effect()) {
                return failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The capability does not match a registered tool effect.");
            }
            lastAuthorization_.emplace(authorizedCall);
            return handleResult.get();
        } catch (...) {
            return Domain::Result<Domain::ToolCallOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic tool handler could not record the call."));
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }


    [[nodiscard]] const std::optional<Contracts::AuthorizedToolCall>&
    lastAuthorization() const noexcept
    {
        return lastAuthorization_;
    }

private:
    [[nodiscard]] static Domain::Result<Domain::ToolCallOutcome> failure(
        const std::string_view code,
        const char* const message)
    {
        return Domain::Result<Domain::ToolCallOutcome>::failure(
            Domain::makeError(code, message));
    }

    std::vector<Domain::McpToolDescriptor> descriptors_;
    std::optional<Contracts::AuthorizedToolCall> lastAuthorization_;
    std::optional<Domain::OperationContext> lastContext_;
    Domain::MonotonicTimePoint now_{};
    std::size_t calls_{};
};

class BoundedToolCatalogFake final : public Contracts::IToolCatalog {
public:
    explicit BoundedToolCatalogFake(
        std::vector<Domain::McpToolDescriptor> descriptors)
        : descriptors_{std::move(descriptors)}
    {
        if (descriptors_.size() > MaximumDeterministicToolDescriptors) {
            throw std::invalid_argument{
                "The deterministic catalog descriptor count is invalid."};
        }
    }

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override
    {
        return descriptors_;
    }

private:
    std::vector<Domain::McpToolDescriptor> descriptors_;
};

class RecordingToolRouterFake final : public Contracts::IToolRouter {
public:
    RecordingToolRouterFake(
        Contracts::IToolAuthorizer& authorizer,
        Contracts::IToolHandler& handler,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : authorizer_{authorizer}, handler_{handler}, now_{now}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastRequest_ = request;
            if (shutdown_ || context.isCancellationRequested() ||
                (cancelledOperation_ &&
                 cancelledOperation_.value() == context.operationId)) {
                return failure(Domain::ErrorCodes::Cancelled,
                    "The deterministic tool invocation was cancelled.");
            }
            if (context.isExpired(now_)) {
                return failure(Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic tool invocation deadline expired.");
            }
            const auto descriptors = handler_.tools();
            const auto descriptor = std::find_if(
                descriptors.begin(),
                descriptors.end(),
                [&](const Domain::McpToolDescriptor& candidate) {
                    return candidate.tool.name == request.toolName;
                });
            if (descriptor == descriptors.end()) {
                return failure(Domain::ErrorCodes::InvalidRequest,
                    "The requested tool is not registered.");
            }
            auto authorized = authorizer_.authorize(
                Domain::ToolAuthorizationRequest{
                    request,
                    descriptor->tool.effect,
                    Domain::AuthorityReference{
                        authority.authorityId(),
                        authority.generation()}},
                authority,
                context);
            if (!authorized) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(
                    std::move(authorized).error());
            }
            return handler_.handle(authorized.value(), authority, context);
        } catch (...) {
            return failure(Domain::ErrorCodes::InternalFailure,
                "The deterministic tool invocation could not be routed.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            cancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void shutdown() noexcept override { shutdown_ = true; }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::ToolCallRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

private:
    [[nodiscard]] static Domain::Result<Domain::ToolCallOutcome> failure(
        const std::string_view code,
        const char* const message)
    {
        return Domain::Result<Domain::ToolCallOutcome>::failure(
            Domain::makeError(code, message));
    }

    Contracts::IToolAuthorizer& authorizer_;
    Contracts::IToolHandler& handler_;
    Domain::MonotonicTimePoint now_{};
    std::optional<Domain::ToolCallRequest> lastRequest_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::size_t calls_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
