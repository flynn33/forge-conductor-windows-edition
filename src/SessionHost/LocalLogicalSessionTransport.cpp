#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
decodeContinuation(
    const Domain::NativeBootstrapRequest& request,
    const Contracts::ContinuityDocument& document)
{
    try {
        const auto& handoff = document.handoff;
        if (document.canonicalUtf8 != request.canonicalHandoffUtf8 ||
            handoff.handoffId != request.handoffId ||
            handoff.operationId != request.operationId ||
            handoff.contentSha256 != request.handoffSha256) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::IntegrityFailure,
                "The logical successor rejected a cross-bound handoff.");
        }
        if (handoff.project.projectId != request.projectId) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The logical successor rejected a handoff for another project.");
        }
        if (!handoff.successorSession ||
            handoff.successorSession->sessionId != request.successorSessionId ||
            (handoff.successorSession->providerSessionId &&
             *handoff.successorSession->providerSessionId !=
                 request.providerSessionId)) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::IntegrityFailure,
                "The logical successor rejected a mismatched successor binding.");
        }
        if (handoff.nextActions.empty()) {
            return failure<Domain::NativeLogicalContinuation>(
                Domain::ErrorCodes::MalformedMessage,
                "The logical successor requires a bounded next action.");
        }
        const auto& first = handoff.nextActions.front();
        return Domain::Result<Domain::NativeLogicalContinuation>::success(
            Domain::NativeLogicalContinuation{
                request.providerSessionId,
                request.handoffId,
                first.order,
                first.action,
                first.command,
                first.successCondition});
    } catch (...) {
        return failure<Domain::NativeLogicalContinuation>(
            Domain::ErrorCodes::InternalFailure,
            "The logical successor could not allocate its continuation state.");
    }
}

[[nodiscard]] bool exactMatch(
    const Domain::NativeLogicalContinuation& left,
    const Domain::NativeLogicalContinuation& right) noexcept
{
    return left.providerSessionId == right.providerSessionId &&
           left.handoffId == right.handoffId &&
           left.sequence == right.sequence && left.action == right.action &&
           left.command == right.command &&
           left.successCondition == right.successCondition;
}

[[nodiscard]] bool exactMatch(
    const Domain::SessionCreationRequest& left,
    const Domain::SessionCreationRequest& right) noexcept
{
    return left.operationId == right.operationId &&
           left.projectId == right.projectId &&
           left.predecessorSessionId == right.predecessorSessionId &&
           left.idempotencyKey == right.idempotencyKey;
}

} // namespace

class LocalLogicalSessionTransport::Impl final {
public:
    struct ContinuationBinding final {
        Domain::NativeLogicalContinuation continuation;
        Domain::SessionId successorSessionId;
        Domain::Sha256Digest handoffSha256;
    };

    struct LogicalSession final {
        Domain::NativeTransportSession transport;
        Domain::SessionCreationRequest creation;
        Domain::OperationId ownerOperationId;
        Domain::HostSessionStatus status{Domain::HostSessionStatus::Creating};
        std::optional<ContinuationBinding> binding;
    };

    class ActiveOperation final {
    public:
        explicit ActiveOperation(Impl& owner) : owner_{owner}
        {
            std::lock_guard lock{owner_.lifecycleMutex};
            if (!owner_.shutdownRequested.load(std::memory_order_acquire)) {
                ++owner_.activeOperations;
                owns_ = true;
            }
        }

        ~ActiveOperation() noexcept
        {
            if (!owns_) {
                return;
            }
            std::lock_guard lock{owner_.lifecycleMutex};
            --owner_.activeOperations;
            owner_.lifecycleChanged.notify_all();
        }

        ActiveOperation(const ActiveOperation&) = delete;
        ActiveOperation& operator=(const ActiveOperation&) = delete;
        ActiveOperation(ActiveOperation&&) = delete;
        ActiveOperation& operator=(ActiveOperation&&) = delete;

        [[nodiscard]] bool owns() const noexcept { return owns_; }

    private:
        Impl& owner_;
        bool owns_{};
    };

    Impl(
        std::shared_ptr<Contracts::IHasher> ownedHasher,
        Contracts::IContinuityDocumentCodec& documentCodec)
        : hasher{std::move(ownedHasher)},
          ownedScheduler{
              std::make_unique<BoundedLogicalContinuationQueue>()},
          scheduler{ownedScheduler.get()},
          codec{&documentCodec}
    {
        validateDependencies();
    }

    Impl(
        std::shared_ptr<Contracts::IHasher> ownedHasher,
        Contracts::IContinuityDocumentCodec& documentCodec,
        Contracts::INativeLogicalContinuationScheduler& continuationScheduler)
        : hasher{std::move(ownedHasher)},
          scheduler{&continuationScheduler},
          codec{&documentCodec}
    {
        validateDependencies();
    }

    void validateDependencies() const
    {
        if (!hasher || scheduler == nullptr || codec == nullptr) {
            throw std::invalid_argument(
                "LocalLogicalSessionTransport requires a hasher, codec, and scheduler.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> create(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            ActiveOperation active{*this};
            if (!active.owns()) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session transport is shutting down.");
            }
            auto valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::NativeTransportSession>(
                    valid.error().code, valid.error().message);
            }
            {
                std::lock_guard lock{mutex};
                valid = validateContext(context, shutdownRequested);
                if (!valid) {
                    return failure<Domain::NativeTransportSession>(
                        valid.error().code, valid.error().message);
                }
                if (cancelled.contains(context.operationId.value())) {
                    return failure<Domain::NativeTransportSession>(
                        Domain::ErrorCodes::Cancelled,
                        "The local logical-session creation was cancelled.");
                }
                const auto existing = sessions.find(
                    request.idempotencyKey.value());
                if (existing != sessions.end()) {
                    if (!exactMatch(existing->second.creation, request)) {
                        return failure<Domain::NativeTransportSession>(
                            Domain::ErrorCodes::Conflict,
                            "The idempotency key is bound to another logical session.");
                    }
                    return Domain::Result<
                        Domain::NativeTransportSession>::success(
                        existing->second.transport);
                }
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
            std::lock_guard lock{mutex};
            valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::NativeTransportSession>(
                    valid.error().code, valid.error().message);
            }
            if (cancelled.contains(context.operationId.value())) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session creation was cancelled.");
            }
            const auto replay = sessions.find(request.idempotencyKey.value());
            if (replay != sessions.end()) {
                if (!exactMatch(replay->second.creation, request)) {
                    return failure<Domain::NativeTransportSession>(
                        Domain::ErrorCodes::Conflict,
                        "The idempotency key is bound to another logical session.");
                }
                return Domain::Result<Domain::NativeTransportSession>::success(
                    replay->second.transport);
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
            if (providerIndex.contains(providerKey)) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::Conflict,
                    "The logical-session identifier collided with another session.");
            }
            const auto& idempotencyKey = request.idempotencyKey.value();
            insertionOrder.push_back(idempotencyKey);
            try {
                providerIndex.emplace(providerKey, idempotencyKey);
                try {
                    sessions.emplace(
                        idempotencyKey,
                        LogicalSession{
                            created,
                            request,
                            context.operationId,
                            Domain::HostSessionStatus::Active,
                            std::nullopt});
                } catch (...) {
                    providerIndex.erase(providerKey);
                    throw;
                }
            } catch (...) {
                insertionOrder.pop_back();
                throw;
            }
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
        try {
            ActiveOperation active{*this};
            if (!active.owns()) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session transport is shutting down.");
            }
            auto valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::NativeBootstrapResponse>(
                    valid.error().code, valid.error().message);
            }
            auto document = codec->decode(
                request.canonicalHandoffUtf8, context);
            if (!document) {
                return failure<Domain::NativeBootstrapResponse>(
                    document.error().code,
                    document.error().message,
                    document.error().retryable);
            }
            auto decoded = decodeContinuation(request, document.value());
            if (!decoded) {
                return failure<Domain::NativeBootstrapResponse>(
                    decoded.error().code,
                    decoded.error().message,
                    decoded.error().retryable);
            }
            auto accepted = acknowledgement(request);
            ContinuationBinding provisional{
                decoded.value(),
                request.successorSessionId,
                request.handoffSha256};
            {
                std::lock_guard lock{mutex};
                valid = validateContext(context, shutdownRequested);
                if (!valid) {
                    return failure<Domain::NativeBootstrapResponse>(
                        valid.error().code, valid.error().message);
                }
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
                if (session.creation.projectId != request.projectId) {
                    return failure<Domain::NativeBootstrapResponse>(
                        Domain::ErrorCodes::ProjectScopeMismatch,
                        "The local logical successor belongs to another project.");
                }
                if (session.creation.operationId != request.operationId ||
                    session.creation.predecessorSessionId !=
                        document.value().handoff.predecessorSession.sessionId) {
                    return failure<Domain::NativeBootstrapResponse>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The local logical successor rejected a session-chain mismatch.");
                }
                if (session.status == Domain::HostSessionStatus::Cancelled) {
                    return failure<Domain::NativeBootstrapResponse>(
                        Domain::ErrorCodes::Cancelled,
                        "The local logical successor was cancelled.");
                }
                if (session.binding) {
                    if (session.binding->successorSessionId !=
                            request.successorSessionId ||
                        session.binding->handoffSha256 !=
                            request.handoffSha256 ||
                        !exactMatch(
                            session.binding->continuation,
                            decoded.value())) {
                        return failure<Domain::NativeBootstrapResponse>(
                            Domain::ErrorCodes::Conflict,
                            "The local logical successor is bound to another handoff.");
                    }
                    if (session.status ==
                        Domain::HostSessionStatus::Bootstrapping) {
                        return failure<Domain::NativeBootstrapResponse>(
                            Domain::ErrorCodes::DatabaseBusy,
                            "The logical continuation is already being scheduled.",
                            true);
                    }
                    if (session.status != Domain::HostSessionStatus::Ready &&
                        session.status != Domain::HostSessionStatus::Sealed) {
                        return failure<Domain::NativeBootstrapResponse>(
                            Domain::ErrorCodes::Conflict,
                            "The logical continuation has an invalid replay state.");
                    }
                    return accepted;
                }
                session.binding = std::move(provisional);
                session.status = Domain::HostSessionStatus::Bootstrapping;
            }

            auto scheduled = scheduler->schedule(decoded.value(), context);
            if (!scheduled) {
                std::lock_guard lock{mutex};
                const auto provider = providerIndex.find(
                    request.providerSessionId.value());
                if (provider != providerIndex.end()) {
                    auto& session = sessions.at(provider->second);
                    if (session.status ==
                            Domain::HostSessionStatus::Bootstrapping &&
                        session.binding &&
                        exactMatch(
                            session.binding->continuation,
                            decoded.value())) {
                        session.binding.reset();
                        session.status = Domain::HostSessionStatus::Active;
                    }
                }
                return failure<Domain::NativeBootstrapResponse>(
                    scheduled.error().code,
                    scheduled.error().message,
                    scheduled.error().retryable);
            }

            bool cancelScheduled{};
            Domain::Error publicationError = Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The logical continuation could not be published safely.");
            {
                std::lock_guard lock{mutex};
                valid = validateContext(context, shutdownRequested);
                if (!valid) {
                    cancelScheduled = true;
                    publicationError = valid.error();
                } else if (cancelled.contains(context.operationId.value())) {
                    cancelScheduled = true;
                    publicationError = Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The local logical-session bootstrap was cancelled.");
                } else {
                    const auto provider = providerIndex.find(
                        request.providerSessionId.value());
                    if (provider == providerIndex.end()) {
                        cancelScheduled = true;
                        publicationError = Domain::makeError(
                            Domain::ErrorCodes::SessionNotFound,
                            "The local logical successor was not found.");
                    } else {
                        auto& session = sessions.at(provider->second);
                        if (session.status ==
                            Domain::HostSessionStatus::Cancelled) {
                            cancelScheduled = true;
                            publicationError = Domain::makeError(
                                Domain::ErrorCodes::Cancelled,
                                "The local logical successor was cancelled.");
                        } else if (!session.binding ||
                                   session.status !=
                                       Domain::HostSessionStatus::Bootstrapping ||
                                   session.binding->successorSessionId !=
                                       request.successorSessionId ||
                                   session.binding->handoffSha256 !=
                                       request.handoffSha256 ||
                                   !exactMatch(
                                       session.binding->continuation,
                                       decoded.value())) {
                            cancelScheduled = true;
                            publicationError = Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The local logical successor is bound to another handoff.");
                        } else {
                            session.status = Domain::HostSessionStatus::Ready;
                        }
                    }
                }
                if (cancelScheduled) {
                    const auto provider = providerIndex.find(
                        request.providerSessionId.value());
                    if (provider != providerIndex.end()) {
                        auto& session = sessions.at(provider->second);
                        if (session.status ==
                                Domain::HostSessionStatus::Bootstrapping &&
                            session.binding &&
                            exactMatch(
                                session.binding->continuation,
                                decoded.value())) {
                            session.binding.reset();
                            session.status = Domain::HostSessionStatus::Active;
                        }
                    }
                }
            }
            if (cancelScheduled) {
                scheduler->cancel(request.providerSessionId);
                return Domain::Result<Domain::NativeBootstrapResponse>::failure(
                    std::move(publicationError));
            }
            return accepted;
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
        try {
            ActiveOperation active{*this};
            if (!active.owns()) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session transport is shutting down.");
            }
            auto valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::HostSessionStatus>(
                    valid.error().code, valid.error().message);
            }
            std::lock_guard lock{mutex};
            valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<Domain::HostSessionStatus>(
                    valid.error().code, valid.error().message);
            }
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
        try {
            ActiveOperation active{*this};
            if (!active.owns()) {
                return failure<std::optional<Domain::NativeLogicalContinuation>>(
                    Domain::ErrorCodes::Cancelled,
                    "The local logical-session transport is shutting down.");
            }
            auto valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<std::optional<Domain::NativeLogicalContinuation>>(
                    valid.error().code, valid.error().message);
            }
            std::lock_guard lock{mutex};
            valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<std::optional<Domain::NativeLogicalContinuation>>(
                    valid.error().code, valid.error().message);
            }
            const auto indexed = providerIndex.find(sessionId.value());
            if (indexed == providerIndex.end()) {
                return failure<std::optional<Domain::NativeLogicalContinuation>>(
                    Domain::ErrorCodes::SessionNotFound,
                    "The local logical continuation was not found.");
            }
            const auto& binding = sessions.at(indexed->second).binding;
            return Domain::Result<
                std::optional<Domain::NativeLogicalContinuation>>::success(
                binding
                    ? std::optional<Domain::NativeLogicalContinuation>{
                          binding->continuation}
                    : std::nullopt);
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
            ActiveOperation active{*this};
            if (!active.owns()) {
                return;
            }
            std::vector<Domain::ProviderSessionId> scheduledSessions;
            {
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
                        scheduledSessions.push_back(
                            session.transport.providerSessionId);
                    }
                }
            }
            for (const auto& scheduledSession : scheduledSessions) {
                scheduler->cancel(scheduledSession);
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        if (!shutdownRequested.exchange(true, std::memory_order_acq_rel)) {
            try {
                std::unique_lock lock{lifecycleMutex};
                lifecycleChanged.wait(
                    lock, [this] { return activeOperations == 0U; });
            } catch (...) {
            }
            scheduler->shutdown();
        }
    }

private:
    [[nodiscard]] static Domain::Result<Domain::NativeBootstrapResponse>
    acknowledgement(const Domain::NativeBootstrapRequest& request)
    {
        const Json document{
            {"handoff_id", request.handoffId.value()},
            {"successor_session_id", request.successorSessionId.value()}};
        const auto encoded = document.dump();
        return Domain::Result<Domain::NativeBootstrapResponse>::success(
            Domain::NativeBootstrapResponse{
                {bytes(encoded)},
                static_cast<std::int64_t>(
                    (request.canonicalHandoffUtf8.size() + 2U) / 3U),
                16});
    }

public:

    std::shared_ptr<Contracts::IHasher> hasher;
    std::unique_ptr<BoundedLogicalContinuationQueue> ownedScheduler;
    Contracts::INativeLogicalContinuationScheduler* scheduler{};
    Contracts::IContinuityDocumentCodec* codec{};
    std::mutex lifecycleMutex;
    std::condition_variable lifecycleChanged;
    std::size_t activeOperations{};
    std::mutex mutex;
    std::unordered_map<std::string, LogicalSession> sessions;
    std::unordered_map<std::string, std::string> providerIndex;
    std::deque<std::string> insertionOrder;
    std::unordered_set<std::string> cancelled;
    std::deque<std::string> cancelledOrder;
    std::atomic_bool shutdownRequested{};
};

LocalLogicalSessionTransport::LocalLogicalSessionTransport(
    std::shared_ptr<Contracts::IHasher> hasher,
    Contracts::IContinuityDocumentCodec& codec)
    : implementation_{std::make_unique<Impl>(std::move(hasher), codec)}
{
}

LocalLogicalSessionTransport::LocalLogicalSessionTransport(
    std::shared_ptr<Contracts::IHasher> hasher,
    Contracts::IContinuityDocumentCodec& codec,
    Contracts::INativeLogicalContinuationScheduler& scheduler)
    : implementation_{
          std::make_unique<Impl>(std::move(hasher), codec, scheduler)}
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
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::SessionHost
