#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::SessionHost {
namespace {

using Json = nlohmann::json;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::atomic_bool& shutdown) noexcept
{
    if (shutdown.load(std::memory_order_acquire) ||
        context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The local logical-session operation was cancelled."));
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The local logical-session operation exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result(text.size());
    std::transform(text.begin(), text.end(), result.begin(), [](const char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return result;
}

[[nodiscard]] Domain::Result<Domain::NativeLogicalContinuation>
decodeContinuation(const Domain::NativeBootstrapRequest& request)
{
    try {
        if (request.canonicalHandoffUtf8.empty() ||
            request.canonicalHandoffUtf8.size() >
                Domain::MaximumContinuityHandoffEncodedBytes ||
            !Domain::isValidUtf8(request.canonicalHandoffUtf8)) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::MalformedMessage,
                "The logical successor received an invalid canonical handoff.");
        }
        const auto document = Json::parse(request.canonicalHandoffUtf8);
        if (!document.is_object() ||
            document.value("handoff_id", std::string{}) !=
                request.handoffId.value() ||
            document.value("operation_id", std::string{}) !=
                request.operationId.value()) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::IntegrityFailure,
                "The logical successor rejected a cross-bound handoff.");
        }
        const auto project = document.find("project");
        if (project == document.end() || !project->is_object() ||
            project->value("project_id", std::string{}) !=
                request.projectId.value()) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The logical successor rejected a handoff for another project.");
        }
        const auto actions = document.find("next_actions");
        if (actions == document.end() || !actions->is_array() ||
            actions->empty() || !actions->front().is_object()) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::MalformedMessage,
                "The logical successor requires a bounded next action.");
        }
        const auto& first = actions->front();
        const auto action = first.value("action", std::string{});
        const auto command = first.value("command", std::string{});
        const auto condition = first.value("success_condition", std::string{});
        if (action.empty() || condition.empty() || action.size() > 4096U ||
            command.size() > 4096U || condition.size() > 4096U ||
            !Domain::isValidUtf8(action) || !Domain::isValidUtf8(command) ||
            !Domain::isValidUtf8(condition)) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::MalformedMessage,
                "The logical successor received an invalid next action.");
        }
        return Domain::Result<Domain::NativeLogicalContinuation>::success(
            Domain::NativeLogicalContinuation{
                request.providerSessionId,
                request.handoffId,
                1U,
                action,
                command,
                condition});
    } catch (const nlohmann::json::exception&) {
        return failure<Domain::NativeLogicalContinuation>(
            Domain::ErrorCodes::MalformedMessage,
            "The logical successor could not decode the canonical handoff.");
    } catch (...) {
        return failure<Domain::NativeLogicalContinuation>(
            Domain::ErrorCodes::InternalFailure,
            "The logical successor could not allocate its continuation state.");
    }
}

} // namespace

class LocalLogicalSessionTransport::Impl final {
public:
    struct LogicalSession final {
        Domain::NativeTransportSession transport;
        Domain::OperationId ownerOperationId;
        Domain::HostSessionStatus status{Domain::HostSessionStatus::Creating};
        std::optional<Domain::NativeLogicalContinuation> continuation;
    };

    explicit Impl(std::shared_ptr<Contracts::IHasher> ownedHasher)
        : hasher{std::move(ownedHasher)}
    {
        if (!hasher) {
            throw std::invalid_argument(
                "LocalLogicalSessionTransport requires a hasher.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> create(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::NativeTransportSession>(
                    valid.error().code, valid.error().message);
            }
            std::lock_guard lock{mutex};
            if (cancelled.contains(context.operationId.value())) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session creation was cancelled.");
            }
            const auto existing = sessions.find(request.idempotencyKey.value());
            if (existing != sessions.end()) {
                return Domain::Result<Domain::NativeTransportSession>::success(
                    existing->second.transport);
            }
            const auto keyBytes = std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(
                    request.idempotencyKey.value().data()),
                request.idempotencyKey.value().size()};
            auto digest = hasher->sha256(keyBytes);
            if (!digest) {
                return failure<Domain::NativeTransportSession>(
                    digest.error().code, digest.error().message);
            }
            auto providerId = Domain::ProviderSessionId::parse(
                "native-" + digest.value().value().substr(0U, 24U), 512U);
            if (!providerId) {
                return failure<Domain::NativeTransportSession>(
                    providerId.error().code, providerId.error().message);
            }
            if (sessions.size() >= Domain::MaximumNativeSessionRecords) {
                auto removable = insertionOrder.end();
                for (auto item = insertionOrder.begin();
                     item != insertionOrder.end(); ++item) {
                    const auto candidate = sessions.find(*item);
                    if (candidate != sessions.end() &&
                        (candidate->second.status ==
                             Domain::HostSessionStatus::Cancelled ||
                         candidate->second.status ==
                             Domain::HostSessionStatus::Failed ||
                         candidate->second.status ==
                             Domain::HostSessionStatus::Sealed)) {
                        removable = item;
                        break;
                    }
                }
                if (removable == insertionOrder.end()) {
                    return failure<Domain::NativeTransportSession>(
                        Domain::ErrorCodes::StorageFull,
                        "The local logical-session mapping reached its configured limit.");
                }
                const auto removed = sessions.find(*removable);
                providerIndex.erase(
                    removed->second.transport.providerSessionId.value());
                sessions.erase(removed);
                insertionOrder.erase(removable);
            }
            Domain::NativeTransportSession created{
                std::move(providerId).value(),
                std::optional<std::string>{"forge-logical-session"}};
            const auto providerKey = created.providerSessionId.value();
            insertionOrder.push_back(request.idempotencyKey.value());
            providerIndex.emplace(providerKey, request.idempotencyKey.value());
            sessions.emplace(
                request.idempotencyKey.value(),
                LogicalSession{
                    created,
                    context.operationId,
                    Domain::HostSessionStatus::Active,
                    std::nullopt});
            return Domain::Result<Domain::NativeTransportSession>::success(
                std::move(created));
        } catch (...) {
            return failure<Domain::NativeTransportSession>(
                Domain::ErrorCodes::InternalFailure,
                "The local logical-session creation failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, shutdownRequested);
        if (!valid) {
            return failure<Domain::NativeBootstrapResponse>(
                valid.error().code, valid.error().message);
        }
        auto decoded = decodeContinuation(request);
        if (!decoded) {
            return failure<Domain::NativeBootstrapResponse>(
                decoded.error().code, decoded.error().message);
        }
        try {
            std::lock_guard lock{mutex};
            if (cancelled.contains(context.operationId.value())) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session bootstrap was cancelled.");
            }
            const auto provider = providerIndex.find(
                request.providerSessionId.value());
            if (provider == providerIndex.end()) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::SessionNotFound,
                    "The local logical successor was not found.");
            }
            auto& session = sessions.at(provider->second);
            if (session.continuation) {
                if (session.continuation->handoffId != request.handoffId) {
                    return failure<Domain::NativeBootstrapResponse>(
                        Domain::ErrorCodes::Conflict,
                        "The local logical successor is bound to another handoff.");
                }
                decoded.value().sequence = session.continuation->sequence;
            }
            session.continuation = decoded.value();
            session.status = Domain::HostSessionStatus::Ready;

            const Json acknowledgement{
                {"handoff_id", request.handoffId.value()},
                {"successor_session_id", request.successorSessionId.value()}};
            const auto encoded = acknowledgement.dump();
            return Domain::Result<Domain::NativeBootstrapResponse>::success(
                Domain::NativeBootstrapResponse{
                    {bytes(encoded)},
                    static_cast<std::int64_t>(
                        (request.canonicalHandoffUtf8.size() + 2U) / 3U),
                    16});
        } catch (...) {
            return failure<Domain::NativeBootstrapResponse>(
                Domain::ErrorCodes::InternalFailure,
                "The local logical-session bootstrap failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, shutdownRequested);
        if (!valid) {
            return failure<Domain::HostSessionStatus>(
                valid.error().code, valid.error().message);
        }
        try {
            std::lock_guard lock{mutex};
            const auto indexed = providerIndex.find(sessionId.value());
            if (indexed == providerIndex.end()) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::SessionNotFound,
                    "The local logical session was not found.");
            }
            return Domain::Result<Domain::HostSessionStatus>::success(
                sessions.at(indexed->second).status);
        } catch (...) {
            return failure<Domain::HostSessionStatus>(
                Domain::ErrorCodes::InternalFailure,
                "The local logical-session query failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::NativeLogicalContinuation>> continuationFor(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, shutdownRequested);
        if (!valid) {
            return failure<std::optional<Domain::NativeLogicalContinuation>>(
                valid.error().code, valid.error().message);
        }
        try {
            std::lock_guard lock{mutex};
            const auto indexed = providerIndex.find(sessionId.value());
            if (indexed == providerIndex.end()) {
                return failure<std::optional<Domain::NativeLogicalContinuation>>(
                    Domain::ErrorCodes::SessionNotFound,
                    "The local logical continuation was not found.");
            }
            return Domain::Result<
                std::optional<Domain::NativeLogicalContinuation>>::success(
                sessions.at(indexed->second).continuation);
        } catch (...) {
            return failure<std::optional<Domain::NativeLogicalContinuation>>(
                Domain::ErrorCodes::InternalFailure,
                "The local logical continuation query failed safely.");
        }
    }

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId) noexcept
    {
        try {
            std::lock_guard lock{mutex};
            if (cancelled.size() >= 256U && !cancelledOrder.empty()) {
                cancelled.erase(cancelledOrder.front());
                cancelledOrder.pop_front();
            }
            if (cancelled.insert(operationId.value()).second) {
                cancelledOrder.push_back(operationId.value());
            }
            for (auto& [key, session] : sessions) {
                static_cast<void>(key);
                if (session.ownerOperationId == operationId ||
                    (sessionId &&
                     session.transport.providerSessionId == *sessionId)) {
                    session.status = Domain::HostSessionStatus::Cancelled;
                }
            }
        } catch (...) {
        }
    }

    std::shared_ptr<Contracts::IHasher> hasher;
    std::mutex mutex;
    std::unordered_map<std::string, LogicalSession> sessions;
    std::unordered_map<std::string, std::string> providerIndex;
    std::deque<std::string> insertionOrder;
    std::unordered_set<std::string> cancelled;
    std::deque<std::string> cancelledOrder;
    std::atomic_bool shutdownRequested{};
};

LocalLogicalSessionTransport::LocalLogicalSessionTransport(
    std::shared_ptr<Contracts::IHasher> hasher)
    : implementation_{std::make_unique<Impl>(std::move(hasher))}
{
}

LocalLogicalSessionTransport::~LocalLogicalSessionTransport() noexcept
{
    shutdown();
}

Domain::Result<Domain::NativeTransportSession>
LocalLogicalSessionTransport::createSession(
    const Domain::SessionCreationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->create(request, context);
}

Domain::Result<Domain::NativeBootstrapResponse>
LocalLogicalSessionTransport::bootstrap(
    const Domain::NativeBootstrapRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->bootstrap(request, context);
}

Domain::Result<Domain::HostSessionStatus>
LocalLogicalSessionTransport::query(
    const Domain::ProviderSessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->query(sessionId, context);
}

Domain::Result<std::optional<Domain::NativeLogicalContinuation>>
LocalLogicalSessionTransport::continuation(
    const Domain::ProviderSessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->continuationFor(sessionId, context);
}

void LocalLogicalSessionTransport::cancel(
    const Domain::OperationId& operationId,
    const std::optional<Domain::ProviderSessionId>& sessionId) noexcept
{
    implementation_->cancel(operationId, sessionId);
}

void LocalLogicalSessionTransport::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdownRequested.store(
            true, std::memory_order_release);
    }
}

} // namespace ForgeConductor::SessionHost
