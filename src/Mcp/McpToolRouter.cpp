#include "ForgeConductor/Mcp/McpToolRouter.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

using ToolOutcomeResult = Domain::Result<Domain::ToolCallOutcome>;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, message, retryable));
}

[[nodiscard]] bool sameDescriptor(
    const Domain::McpToolDescriptor& left,
    const Domain::McpToolDescriptor& right) noexcept
{
    return left.tool.name == right.tool.name &&
        left.tool.description == right.tool.description &&
        left.tool.pack == right.tool.pack &&
        left.tool.effect == right.tool.effect &&
        left.tool.availability == right.tool.availability &&
        left.tool.requiresProject == right.tool.requiresProject &&
        left.tool.requiresShell == right.tool.requiresShell &&
        left.inputSchema == right.inputSchema;
}

[[nodiscard]] Domain::FileAccess accessFor(
    const Domain::ToolEffect effect) noexcept
{
    switch (effect) {
    case Domain::ToolEffect::Read:
        return Domain::FileAccess::Read;
    case Domain::ToolEffect::Write:
        return Domain::FileAccess::Write;
    case Domain::ToolEffect::Execute:
        return Domain::FileAccess::Execute;
    case Domain::ToolEffect::Destructive:
        return Domain::FileAccess::Delete;
    }
    return Domain::FileAccess::Read;
}

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess expected) noexcept
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

[[nodiscard]] bool intentAllowsEffect(
    const Domain::FileAccess intent,
    const Domain::ToolEffect effect) noexcept
{
    switch (effect) {
    case Domain::ToolEffect::Read:
        return intent == Domain::FileAccess::Read ||
            intent == Domain::FileAccess::Write;
    case Domain::ToolEffect::Write:
        return intent == Domain::FileAccess::Write;
    case Domain::ToolEffect::Execute:
        return intent == Domain::FileAccess::Execute;
    case Domain::ToolEffect::Destructive:
        return intent == Domain::FileAccess::Write ||
            intent == Domain::FileAccess::Delete;
    }
    return false;
}

[[nodiscard]] bool isPolicyDenial(const std::string_view code) noexcept
{
    return code == Domain::ErrorCodes::Unauthorized ||
        code == Domain::ErrorCodes::ProjectScopeMismatch ||
        code == Domain::ErrorCodes::PathOutsideAuthority ||
        code == Domain::ErrorCodes::ShellDisabled;
}

[[nodiscard]] Domain::MonotonicTimePoint auditDeadline(
    const Domain::MonotonicTimePoint now) noexcept
{
    constexpr auto Grace = std::chrono::seconds{1};
    const auto remaining = Domain::MonotonicTimePoint::max() - now;
    return remaining < Grace ? Domain::MonotonicTimePoint::max() : now + Grace;
}

} // namespace

class McpToolRouter::Implementation final {
public:
    Implementation(
        Contracts::IToolCatalog& catalog,
        Contracts::IToolAuthorizer& authorizer,
        Contracts::IToolInvocationGuard& invocationGuard,
        Contracts::IAuditRepository& auditRepository,
        Contracts::IHasher& hasher,
        Contracts::IClock& clock)
        : catalog_{catalog},
          authorizer_{authorizer},
          invocationGuard_{invocationGuard},
          auditRepository_{auditRepository},
          hasher_{hasher},
          clock_{clock}
    {
        registrations_.reserve(MaximumRegisteredTools);
        activeOperations_.reserve(MaximumActiveOperations);
    }

    [[nodiscard]] Domain::Result<void> initialize(
        const std::span<Contracts::IToolHandler* const> handlers) noexcept
    {
        try {
            const auto catalogTools = catalog_.tools();
            if (catalogTools.empty() ||
                catalogTools.size() > MaximumRegisteredTools ||
                handlers.empty() || handlers.size() > MaximumRegisteredTools) {
                return failure<void>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The MCP router catalog or handler set is empty or exceeds its bound.");
            }

            std::size_t catalogBytes{};
            for (std::size_t index{}; index < catalogTools.size(); ++index) {
                const auto& candidate = catalogTools[index];
                const std::array<std::string_view, 4U> text{
                    candidate.tool.name,
                    candidate.tool.description,
                    candidate.tool.pack,
                    candidate.inputSchema};
                for (const auto value : text) {
                    if (value.size() > MaximumCatalogBytes - catalogBytes) {
                        return failure<void>(
                            Domain::ErrorCodes::LimitExceeded,
                            "The MCP router catalog exceeds its encoded-byte bound.");
                    }
                    catalogBytes += value.size();
                }
                auto valid = Domain::validateToolDescriptor(
                    candidate.tool);
                if (!valid || candidate.inputSchema.empty() ||
                    candidate.tool.description.find('\0') != std::string::npos ||
                    candidate.tool.pack.find('\0') != std::string::npos ||
                    candidate.inputSchema.find('\0') != std::string::npos ||
                    !Domain::isValidUtf8(candidate.tool.description) ||
                    !Domain::isValidUtf8(candidate.tool.pack) ||
                    !Domain::isValidUtf8(candidate.inputSchema)) {
                    return failure<void>(
                        Domain::ErrorCodes::InvalidRequest,
                        "The MCP router catalog contains an invalid descriptor.");
                }
                for (std::size_t prior{}; prior < index; ++prior) {
                    if (catalogTools[prior].tool.name ==
                        catalogTools[index].tool.name) {
                        return failure<void>(
                            Domain::ErrorCodes::Conflict,
                            "The MCP router catalog contains a duplicate tool name.");
                    }
                }
            }

            for (auto* const handler : handlers) {
                if (handler == nullptr) {
                    return failure<void>(
                        Domain::ErrorCodes::InvalidRequest,
                        "The MCP router received a null tool handler.");
                }
                const auto handledTools = handler->tools();
                if (handledTools.empty() ||
                    handledTools.size() > MaximumRegisteredTools) {
                    return failure<void>(
                        Domain::ErrorCodes::LimitExceeded,
                        "An MCP tool handler owns no tools or exceeds the registration bound.");
                }
                for (const auto& handled : handledTools) {
                    const auto catalogEntry = std::find_if(
                        catalogTools.begin(),
                        catalogTools.end(),
                        [&](const Domain::McpToolDescriptor& candidate) {
                            return candidate.tool.name == handled.tool.name;
                        });
                    if (catalogEntry == catalogTools.end() ||
                        !sameDescriptor(*catalogEntry, handled)) {
                        return failure<void>(
                            Domain::ErrorCodes::IntegrityFailure,
                            "An MCP handler descriptor does not exactly match the catalog.");
                    }
                    const auto duplicate = std::find_if(
                        registrations_.begin(),
                        registrations_.end(),
                        [&](const Registration& registered) {
                            return registered.descriptor.tool.name ==
                                handled.tool.name;
                        });
                    if (duplicate != registrations_.end()) {
                        return failure<void>(
                            Domain::ErrorCodes::Conflict,
                            "More than one MCP handler registered the same tool.");
                    }
                    registrations_.push_back(
                        Registration{*catalogEntry, handler});
                }
            }

            if (registrations_.size() != catalogTools.size()) {
                return failure<void>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The MCP handler set does not own every catalog tool exactly once.");
            }
            std::sort(
                registrations_.begin(),
                registrations_.end(),
                [](const Registration& left, const Registration& right) {
                    return left.descriptor.tool.name < right.descriptor.tool.name;
                });
            return Domain::Result<void>::success();
        } catch (...) {
            registrations_.clear();
            return failure<void>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP handler registration map could not be created.");
        }
    }

    [[nodiscard]] ToolOutcomeResult invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto activeResult = admit(context.operationId);
            if (!activeResult) {
                return ToolOutcomeResult::failure(
                    std::move(activeResult).error());
            }
            auto active = std::move(activeResult).value();
            ActiveLease lease{*this, active};
            const std::stop_callback parentCancellation{
                context.cancellation,
                [active] { active->cancellation.request_stop(); }};
            const Domain::OperationContext routedContext{
                context.operationId,
                context.deadline,
                active->cancellation.get_token(),
                context.correlationId};
            const auto started = clock_.monotonicNow();

            auto basicValidation = validateBasicRequest(request, routedContext);
            if (!basicValidation) {
                auto rejected = ToolOutcomeResult::failure(
                    std::move(basicValidation).error());
                recordAudit(request, std::nullopt, started, rejected, routedContext);
                return rejected;
            }

            auto digest = hashArguments(request.canonicalArguments);
            if (!digest) {
                auto rejected = ToolOutcomeResult::failure(
                    std::move(digest).error());
                recordAudit(request, std::nullopt, started, rejected, routedContext);
                return rejected;
            }
            const std::optional<Domain::Sha256Digest> argumentsDigest{
                std::move(digest).value()};

            const auto* const registration = registrationFor(request.toolName);
            if (registration == nullptr) {
                auto rejected = failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::InvalidRequest,
                    "The requested MCP tool is not registered.");
                recordAudit(
                    request, argumentsDigest, started, rejected, routedContext);
                return rejected;
            }

            auto current = validateContext(routedContext);
            if (!current) {
                auto rejected = ToolOutcomeResult::failure(
                    std::move(current).error());
                recordAudit(
                    request, argumentsDigest, started, rejected, routedContext);
                return rejected;
            }

            auto admission = invocationGuard_.beforeInvoke(
                request, registration->descriptor.tool, routedContext);
            if (!admission) {
                invocationGuard_.cancel(routedContext.operationId);
                auto rejected = ToolOutcomeResult::failure(
                    std::move(admission).error());
                recordAudit(
                    request, argumentsDigest, started, rejected, routedContext);
                return rejected;
            }

            current = validateContext(routedContext);
            if (!current) {
                invocationGuard_.cancel(routedContext.operationId);
                auto rejected = ToolOutcomeResult::failure(
                    std::move(current).error());
                recordAudit(
                    request, argumentsDigest, started, rejected, routedContext);
                return rejected;
            }

            auto admissionValue = std::move(admission).value();
            ToolOutcomeResult outcome = admissionValue.immediateOutcome
                ? ToolOutcomeResult::success(std::move(
                      admissionValue.immediateOutcome).value())
                : authorizeDispatchAndObserve(
                      request, *registration, authority, routedContext);

            current = validateContext(routedContext);
            if (!current) {
                outcome = ToolOutcomeResult::failure(
                    std::move(current).error());
            }
            auto validOutcome = validateOutcome(request, outcome);
            if (!validOutcome) {
                outcome = ToolOutcomeResult::failure(
                    std::move(validOutcome).error());
            }
            recordAudit(
                request, argumentsDigest, started, outcome, routedContext);
            return outcome;
        } catch (...) {
            invocationGuard_.cancel(context.operationId);
            return failure<Domain::ToolCallOutcome>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP tool router failed safely at its boundary.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::shared_ptr<ActiveOperation> active;
            {
                std::lock_guard lock{stateMutex_};
                const auto found = std::find_if(
                    activeOperations_.begin(),
                    activeOperations_.end(),
                    [&](const auto& candidate) {
                        return candidate->operationId == operationId;
                    });
                if (found != activeOperations_.end()) {
                    active = *found;
                }
            }
            if (active) {
                active->cancellation.request_stop();
            }
            invocationGuard_.cancel(operationId);
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            std::array<std::shared_ptr<ActiveOperation>,
                       MaximumActiveOperations> active;
            std::size_t count{};
            {
                std::lock_guard lock{stateMutex_};
                if (stopping_) {
                    return;
                }
                stopping_ = true;
                count = activeOperations_.size();
                for (std::size_t index{}; index < count; ++index) {
                    active[index] = activeOperations_[index];
                }
            }
            for (std::size_t index{}; index < count; ++index) {
                active[index]->cancellation.request_stop();
                invocationGuard_.cancel(active[index]->operationId);
            }
            invocationGuard_.shutdown();
        } catch (...) {
            try {
                invocationGuard_.shutdown();
            } catch (...) {
            }
        }
    }

    void waitUntilIdle() noexcept
    {
        try {
            std::unique_lock lock{stateMutex_};
            stateChanged_.wait(lock, [&] { return activeOperations_.empty(); });
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t activeOperationCount() const noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            return activeOperations_.size();
        } catch (...) {
            return 0U;
        }
    }

private:
    struct Registration final {
        Domain::McpToolDescriptor descriptor;
        Contracts::IToolHandler* handler{};
    };

    struct ActiveOperation final {
        explicit ActiveOperation(Domain::OperationId id)
            : operationId{std::move(id)}
        {
        }

        Domain::OperationId operationId;
        std::stop_source cancellation;
    };

    class ActiveLease final {
    public:
        ActiveLease(
            Implementation& owner,
            std::shared_ptr<ActiveOperation> active) noexcept
            : owner_{owner}, active_{std::move(active)}
        {
        }

        ~ActiveLease() noexcept { owner_.release(active_); }

        ActiveLease(const ActiveLease&) = delete;
        ActiveLease& operator=(const ActiveLease&) = delete;
        ActiveLease(ActiveLease&&) = delete;
        ActiveLease& operator=(ActiveLease&&) = delete;

    private:
        Implementation& owner_;
        std::shared_ptr<ActiveOperation> active_;
    };

    [[nodiscard]] Domain::Result<std::shared_ptr<ActiveOperation>> admit(
        const Domain::OperationId& operationId)
    {
        auto active = std::make_shared<ActiveOperation>(operationId);
        std::lock_guard lock{stateMutex_};
        if (stopping_) {
            return failure<std::shared_ptr<ActiveOperation>>(
                Domain::ErrorCodes::TransportClosed,
                "The MCP tool router is shutting down.");
        }
        const auto duplicate = std::find_if(
            activeOperations_.begin(),
            activeOperations_.end(),
            [&](const auto& candidate) {
                return candidate->operationId == operationId;
            });
        if (duplicate != activeOperations_.end()) {
            return failure<std::shared_ptr<ActiveOperation>>(
                Domain::ErrorCodes::OwnershipConflict,
                "The MCP operation identifier is already active.");
        }
        if (activeOperations_.size() >= MaximumActiveOperations) {
            return failure<std::shared_ptr<ActiveOperation>>(
                Domain::ErrorCodes::LimitExceeded,
                "The MCP router active-operation bound was reached.",
                true);
        }
        activeOperations_.push_back(active);
        return Domain::Result<std::shared_ptr<ActiveOperation>>::success(
            std::move(active));
    }

    void release(const std::shared_ptr<ActiveOperation>& active) noexcept
    {
        try {
            {
                std::lock_guard lock{stateMutex_};
                const auto found = std::find(
                    activeOperations_.begin(), activeOperations_.end(), active);
                if (found != activeOperations_.end()) {
                    activeOperations_.erase(found);
                }
            }
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] const Registration* registrationFor(
        const std::string_view toolName) const noexcept
    {
        const auto found = std::lower_bound(
            registrations_.begin(),
            registrations_.end(),
            toolName,
            [](const Registration& candidate, const std::string_view name) {
                return candidate.descriptor.tool.name < name;
            });
        return found != registrations_.end() &&
                found->descriptor.tool.name == toolName
            ? &*found
            : nullptr;
    }

    [[nodiscard]] Domain::Result<void> validateBasicRequest(
        const Domain::ToolCallRequest& request,
        const Domain::OperationContext& context) const noexcept
    {
        if (request.toolName.empty() ||
            request.toolName.size() > MaximumToolNameBytes ||
            !Domain::isValidUtf8(request.toolName) ||
            request.canonicalArguments.empty() ||
            request.canonicalArguments.size() > MaximumCanonicalArgumentsBytes ||
            request.canonicalArguments.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(request.canonicalArguments) ||
            request.metadata.protocolVersion.empty() ||
            request.metadata.protocolVersion.size() >
                MaximumProtocolVersionBytes ||
            request.metadata.protocolVersion.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(request.metadata.protocolVersion)) {
            return failure<void>(
                Domain::ErrorCodes::InvalidRequest,
                "The MCP tool request is incomplete or exceeds its bounds.");
        }
        if (request.metadata.correlationId != context.correlationId) {
            return failure<void>(
                Domain::ErrorCodes::Unauthorized,
                "The MCP tool request correlation does not match its operation.");
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> validateAuthority(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor& descriptor,
        const Contracts::WorkspaceAuthority& authority) const noexcept
    {
        if (request.metadata.clientId != authority.callerId()) {
            return failure<void>(
                Domain::ErrorCodes::Unauthorized,
                "The MCP caller does not match the resolved authority.");
        }
        if (request.metadata.projectId &&
            request.metadata.projectId.value() != authority.projectId()) {
            return failure<void>(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The MCP project does not match the resolved authority.");
        }
        if (descriptor.requiresProject && authority.trustedRoots().empty()) {
            return failure<void>(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The MCP tool requires a resolved project authority.");
        }

        const auto requiredAccess = accessFor(descriptor.effect);
        if (!intentAllowsEffect(authority.intent(), descriptor.effect) ||
            !containsAccess(authority.grants(), requiredAccess) ||
            containsAccess(authority.denials(), requiredAccess)) {
            return failure<void>(
                Domain::ErrorCodes::Unauthorized,
                "The resolved authority does not grant the requested tool effect.");
        }
        if (descriptor.requiresShell &&
            (!authority.shellEnabled() ||
             !containsAccess(
                 authority.grants(), Domain::FileAccess::Execute) ||
             containsAccess(
                 authority.denials(), Domain::FileAccess::Execute))) {
            return failure<void>(
                Domain::ErrorCodes::ShellDisabled,
                "Shell execution is not enabled by the resolved authority.");
        }
        if (descriptor.availability != Domain::ToolAvailability::Available) {
            return failure<void>(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The requested MCP tool is not currently available.");
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept
    {
        if (context.isCancellationRequested()) {
            return failure<void>(
                Domain::ErrorCodes::Cancelled,
                "The MCP tool invocation was cancelled.");
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return failure<void>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The MCP tool invocation exceeded its deadline.");
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> hashArguments(
        const std::string& canonicalArguments) noexcept
    {
        try {
            const auto characters = std::span{
                canonicalArguments.data(), canonicalArguments.size()};
            return hasher_.sha256(std::as_bytes(characters));
        } catch (...) {
            return failure<Domain::Sha256Digest>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP audit argument digest could not be created.");
        }
    }

    [[nodiscard]] ToolOutcomeResult authorizeDispatchAndObserve(
        const Domain::ToolCallRequest& request,
        const Registration& registration,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept
    {
        ToolOutcomeResult outcome = [&]() -> ToolOutcomeResult {
            try {
                auto authorityValidation = validateAuthority(
                    request, registration.descriptor.tool, authority);
                if (!authorityValidation) {
                    return ToolOutcomeResult::failure(
                        std::move(authorityValidation).error());
                }
                auto scopedRequest = request;
                if (registration.descriptor.tool.requiresProject &&
                    !scopedRequest.metadata.projectId) {
                    scopedRequest.metadata.projectId = authority.projectId();
                }
                auto authorized = authorizer_.authorize(
                    Domain::ToolAuthorizationRequest{
                        scopedRequest,
                        registration.descriptor.tool.effect,
                        Domain::AuthorityReference{
                            authority.authorityId(), authority.generation()}},
                    authority,
                    context);
                if (!authorized) {
                    return ToolOutcomeResult::failure(
                        std::move(authorized).error());
                }
                if (!authorized.value().matches(scopedRequest) ||
                    !authorized.value().matches(authority, context) ||
                    authorized.value().effect() !=
                        registration.descriptor.tool.effect) {
                    return failure<Domain::ToolCallOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The tool authorizer returned a mismatched capability.");
                }
                auto current = validateContext(context);
                if (!current) {
                    return ToolOutcomeResult::failure(
                        std::move(current).error());
                }
                return registration.handler->handle(
                    authorized.value(), authority, context);
            } catch (...) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::InternalFailure,
                    "The resolved MCP tool scope could not be dispatched.");
            }
        }();

        auto current = validateContext(context);
        if (!current) {
            outcome = ToolOutcomeResult::failure(
                std::move(current).error());
        }
        return invocationGuard_.afterInvoke(
            request,
            registration.descriptor.tool,
            std::move(outcome),
            context);
    }

    [[nodiscard]] Domain::Result<void> validateOutcome(
        const Domain::ToolCallRequest& request,
        const ToolOutcomeResult& outcome) const noexcept
    {
        if (!outcome) {
            return Domain::Result<void>::success();
        }
        const auto& value = outcome.value();
        if (value.receipt.requestId != request.metadata.requestId ||
            value.receipt.toolName != request.toolName ||
            value.receipt.ok == value.receipt.error.has_value() ||
            value.canonicalPayload.empty() ||
            value.canonicalPayload.size() > MaximumCanonicalArgumentsBytes ||
            value.canonicalPayload.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(value.canonicalPayload)) {
            return failure<void>(
                Domain::ErrorCodes::IntegrityFailure,
                "The MCP handler or invocation guard returned an invalid outcome.");
        }
        return Domain::Result<void>::success();
    }

    void recordAudit(
        const Domain::ToolCallRequest& request,
        const std::optional<Domain::Sha256Digest>& argumentsDigest,
        const Domain::MonotonicTimePoint started,
        const ToolOutcomeResult& outcome,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const auto finished = clock_.monotonicNow();
            const auto elapsed = finished > started
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      finished - started)
                : std::chrono::milliseconds{};
            std::optional<std::string> error;
            std::string status{"ok"};
            if (!outcome) {
                error = outcome.error().code;
                status = isPolicyDenial(*error) ? "denied" : "error";
            } else if (outcome.value().receipt.error) {
                error = outcome.value().receipt.error->code;
                status = isPolicyDenial(*error) ? "denied" : "error";
            }
            const Domain::AuditEvent event{
                clock_.utcNow(),
                request.metadata.clientId,
                request.toolName,
                argumentsDigest,
                std::move(status),
                elapsed,
                std::move(error)};
            const auto now = clock_.monotonicNow();
            const Domain::OperationContext auditContext{
                context.operationId,
                auditDeadline(now),
                {},
                context.correlationId};
            static_cast<void>(auditRepository_.append(event, auditContext));
        } catch (...) {
            // Audit failure cannot turn a completed mutation into a retryable
            // transport failure or expose unsanitized arguments at this boundary.
        }
    }

    Contracts::IToolCatalog& catalog_;
    Contracts::IToolAuthorizer& authorizer_;
    Contracts::IToolInvocationGuard& invocationGuard_;
    Contracts::IAuditRepository& auditRepository_;
    Contracts::IHasher& hasher_;
    Contracts::IClock& clock_;
    std::vector<Registration> registrations_;

    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::vector<std::shared_ptr<ActiveOperation>> activeOperations_;
    bool stopping_{};
};

Domain::Result<std::unique_ptr<McpToolRouter>> McpToolRouter::create(
    Contracts::IToolCatalog& catalog,
    const std::span<Contracts::IToolHandler* const> handlers,
    Contracts::IToolAuthorizer& authorizer,
    Contracts::IToolInvocationGuard& invocationGuard,
    Contracts::IAuditRepository& auditRepository,
    Contracts::IHasher& hasher,
    Contracts::IClock& clock) noexcept
{
    try {
        auto implementation = std::make_unique<Implementation>(
            catalog,
            authorizer,
            invocationGuard,
            auditRepository,
            hasher,
            clock);
        auto initialized = implementation->initialize(handlers);
        if (!initialized) {
            return Domain::Result<std::unique_ptr<McpToolRouter>>::failure(
                std::move(initialized).error());
        }
        return Domain::Result<std::unique_ptr<McpToolRouter>>::success(
            std::unique_ptr<McpToolRouter>{
                new McpToolRouter{std::move(implementation)}});
    } catch (...) {
        return failure<std::unique_ptr<McpToolRouter>>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP tool router could not be created.");
    }
}

McpToolRouter::McpToolRouter(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

McpToolRouter::~McpToolRouter() noexcept
{
    shutdown();
    if (implementation_) {
        implementation_->waitUntilIdle();
    }
}

Domain::Result<Domain::ToolCallOutcome> McpToolRouter::invoke(
    const Domain::ToolCallRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ToolCallOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP tool router has no implementation.");
    }
    return implementation_->invoke(request, authority, context);
}

void McpToolRouter::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (implementation_) {
        implementation_->cancel(operationId);
    }
}

void McpToolRouter::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

std::size_t McpToolRouter::activeOperationCount() const noexcept
{
    return implementation_ ? implementation_->activeOperationCount() : 0U;
}

} // namespace ForgeConductor::Mcp
