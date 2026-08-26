#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
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

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagate(Domain::Result<U> source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const std::atomic_bool& shutdown) noexcept
{
    if (shutdown.load(std::memory_order_acquire) ||
        context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The native session-host operation was cancelled."));
    }
    if (context.isExpired(clock.monotonicNow())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The native session-host operation exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateProvider(
    const Domain::NativeTransportSession& session)
{
    auto identifier = Domain::validateOpaqueIdentifier(
        session.providerSessionId.value(), 512U);
    if (!identifier || session.providerSessionId.value().find('\r') !=
            std::string::npos ||
        session.providerSessionId.value().find('\n') != std::string::npos) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The native provider returned an invalid session identifier."));
    }
    if (session.model &&
        (session.model->size() > 256U ||
         !Domain::isValidUtf8(*session.model))) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The native provider returned an invalid model name."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateAcknowledgement(
    const Domain::NativeBootstrapResponse& response,
    const Domain::ContinuityHandoffId& handoffId,
    const Domain::SessionId& successorSessionId)
{
    if (response.chunks.empty() ||
        response.chunks.size() > Domain::MaximumNativeResponseChunks) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The native acknowledgement chunk count is outside its bound."));
    }
    if (response.inputTokens < 0 || response.outputTokens < 0) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The native acknowledgement reported negative usage."));
    }
    try {
        std::size_t total{};
        std::string payload;
        for (const auto& chunk : response.chunks) {
            if (chunk.size() > Domain::MaximumNativeResponseChunkBytes ||
                total > Domain::MaximumNativeResponseBytes - chunk.size()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The native acknowledgement exceeds its streaming bounds."));
            }
            total += chunk.size();
            payload.append(
                reinterpret_cast<const char*>(chunk.data()),
                chunk.size());
        }
        if (!Domain::isValidUtf8(payload)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::MalformedMessage,
                "The native acknowledgement is not valid UTF-8."));
        }
        const auto value = Json::parse(payload);
        if (!value.is_object() || value.size() != 2U ||
            value.value("handoff_id", std::string{}) != handoffId.value() ||
            value.value("successor_session_id", std::string{}) !=
                successorSessionId.value()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The native acknowledgement does not match the exact handoff and successor."));
        }
        return Domain::Result<void>::success();
    } catch (const nlohmann::json::exception&) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The native acknowledgement is not valid JSON."));
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native acknowledgement could not be decoded."));
    }
}

[[nodiscard]] Domain::CorrelationId cancelCorrelationId()
{
    auto parsed = Domain::CorrelationId::parse("native-session-host-cancel");
    if (!parsed) {
        throw std::logic_error("The built-in cancellation correlation ID is invalid.");
    }
    return std::move(parsed).value();
}

} // namespace

class ForgeNativeSessionHostAdapter::Impl final {
public:
    Impl(
        Domain::AdapterId ownedAdapterId,
        Contracts::INativeSessionLedger& ownedLedger,
        Contracts::INativeSessionTransport& ownedTransport,
        Contracts::IContinuityDocumentCodec& ownedCodec,
        Contracts::IUuidGenerator& ownedUuidGenerator,
        Contracts::IClock& ownedClock)
        : adapterId{std::move(ownedAdapterId)},
          ledgerStore{ownedLedger},
          transport{ownedTransport},
          codec{ownedCodec},
          uuidGenerator{ownedUuidGenerator},
          clock{ownedClock}
    {
        if (adapterId.value() != AdapterIdentifier) {
            throw std::invalid_argument(
                "ForgeNativeSessionHostAdapter requires its canonical adapter ID.");
        }
    }

    [[nodiscard]] Domain::Result<void> ensureLoadedLocked(
        const Domain::OperationContext& context)
    {
        if (loaded) {
            return Domain::Result<void>::success();
        }
        auto result = ledgerStore.load(context);
        if (!result) {
            return propagate<void>(std::move(result));
        }
        auto valid = Domain::validateNativeSessionLedger(result.value());
        if (!valid) {
            return valid;
        }
        ledger = std::move(result).value();
        loaded = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> commitLocked(
        const Domain::OperationContext& context)
    {
        auto valid = Domain::validateNativeSessionLedger(ledger);
        if (!valid) {
            return valid;
        }
        auto committed = ledgerStore.commit(ledger, ledger.revision, context);
        if (!committed) {
            return propagate<void>(std::move(committed));
        }
        ledger = std::move(committed).value();
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::vector<Domain::NativeSessionRecord>::iterator
    findByKeyLocked(const Domain::IdempotencyKey& key)
    {
        return std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [&key](const auto& record) {
                return record.session.idempotencyKey == key;
            });
    }

    [[nodiscard]] std::vector<Domain::NativeSessionRecord>::iterator
    findBySessionLocked(const Domain::SessionId& id)
    {
        return std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [&id](const auto& record) { return record.session.id == id; });
    }

    [[nodiscard]] Domain::Result<Domain::HostSession> create(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<Domain::HostSession>(std::move(valid));
            }
            Domain::SessionId logicalId = request.predecessorSessionId;
            {
                std::lock_guard lock{mutex};
                valid = ensureLoadedLocked(context);
                if (!valid) {
                    return propagate<Domain::HostSession>(std::move(valid));
                }
                auto existing = findByKeyLocked(request.idempotencyKey);
                if (existing != ledger.records.end()) {
                    valid = Domain::validateHostSessionBinding(
                        existing->session, request);
                    if (!valid) {
                        return propagate<Domain::HostSession>(std::move(valid));
                    }
                    if (existing->session.status ==
                        Domain::HostSessionStatus::Cancelled) {
                        return failure<Domain::HostSession>(
                            Domain::ErrorCodes::Cancelled,
                            "The durable native session creation was cancelled.");
                    }
                    logicalId = existing->session.id;
                    if (existing->session.providerSessionId) {
                        return Domain::Result<Domain::HostSession>::success(
                            existing->session);
                    }
                } else {
                    if (ledger.records.size() >=
                        Domain::MaximumNativeSessionRecords) {
                        const auto removable = std::find_if(
                            ledger.records.begin(), ledger.records.end(),
                            [](const auto& record) {
                                return record.session.status ==
                                           Domain::HostSessionStatus::Sealed ||
                                    record.session.status ==
                                        Domain::HostSessionStatus::Cancelled ||
                                    record.session.status ==
                                        Domain::HostSessionStatus::Failed;
                            });
                        if (removable == ledger.records.end()) {
                            return failure<Domain::HostSession>(
                                Domain::ErrorCodes::StorageFull,
                                "The native session ledger reached its configured limit.");
                        }
                        ledger.records.erase(removable);
                    }
                    auto uuid = uuidGenerator.next();
                    if (!uuid) {
                        return propagate<Domain::HostSession>(std::move(uuid));
                    }
                    logicalId = Domain::SessionId{std::move(uuid).value()};
                    const auto now = clock.utcNow();
                    ledger.records.push_back(Domain::NativeSessionRecord{
                        Domain::HostSession{
                            logicalId,
                            request.projectId,
                            request.operationId,
                            request.predecessorSessionId,
                            request.idempotencyKey,
                            std::nullopt,
                            std::nullopt,
                            Domain::HostSessionStatus::Creating},
                        context.operationId,
                        std::nullopt,
                        std::nullopt,
                        0U,
                        0U,
                        now,
                        now});
                    valid = commitLocked(context);
                    if (!valid) {
                        return propagate<Domain::HostSession>(std::move(valid));
                    }
                }
            }

            Domain::Result<Domain::NativeTransportSession> provider =
                failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::RateLimited,
                    "The native provider rate limit remained active after bounded retries.",
                    true);
            for (std::size_t attempt = 0U; attempt < MaximumRetries; ++attempt) {
                valid = validateContext(context, clock, shutdownRequested);
                if (!valid) {
                    return propagate<Domain::HostSession>(std::move(valid));
                }
                provider = transport.createSession(request, context);
                if (provider || provider.error().code !=
                                    Domain::ErrorCodes::RateLimited ||
                    attempt + 1U == MaximumRetries) {
                    break;
                }
            }
            if (!provider) {
                return propagate<Domain::HostSession>(std::move(provider));
            }
            valid = validateProvider(provider.value());
            if (!valid) {
                transport.cancel(
                    context.operationId,
                    std::optional<Domain::ProviderSessionId>{
                        provider.value().providerSessionId});
                return propagate<Domain::HostSession>(std::move(valid));
            }

            std::lock_guard lock{mutex};
            valid = ensureLoadedLocked(context);
            if (!valid) {
                return propagate<Domain::HostSession>(std::move(valid));
            }
            auto existing = findByKeyLocked(request.idempotencyKey);
            if (existing == ledger.records.end() ||
                existing->session.id != logicalId) {
                return failure<Domain::HostSession>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The native session intent disappeared before provider publication.");
            }
            if (cancelledOperations.contains(context.operationId.value()) ||
                context.isCancellationRequested()) {
                transport.cancel(
                    context.operationId,
                    std::optional<Domain::ProviderSessionId>{
                        provider.value().providerSessionId});
                existing->session.status = Domain::HostSessionStatus::Cancelled;
                existing->updatedAt = clock.utcNow();
                static_cast<void>(commitLocked(context));
                return failure<Domain::HostSession>(
                    Domain::ErrorCodes::Cancelled,
                    "The native session was cancelled after provider creation.");
            }
            if (existing->session.providerSessionId &&
                *existing->session.providerSessionId !=
                    provider.value().providerSessionId) {
                return failure<Domain::HostSession>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The idempotent provider returned another physical session.");
            }
            existing->session.providerSessionId =
                provider.value().providerSessionId;
            existing->session.model = provider.value().model;
            existing->session.status = Domain::HostSessionStatus::Active;
            existing->updatedAt = clock.utcNow();
            const auto published = existing->session;
            valid = commitLocked(context);
            if (!valid) {
                return propagate<Domain::HostSession>(std::move(valid));
            }
            return Domain::Result<Domain::HostSession>::success(published);
        } catch (...) {
            return failure<Domain::HostSession>(
                Domain::ErrorCodes::InternalFailure,
                "The native session-host creation failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::HostSession>> byKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<std::optional<Domain::HostSession>>(
                    std::move(valid));
            }
            std::lock_guard lock{mutex};
            valid = ensureLoadedLocked(context);
            if (!valid) {
                return propagate<std::optional<Domain::HostSession>>(
                    std::move(valid));
            }
            const auto existing = findByKeyLocked(key);
            if (existing == ledger.records.end()) {
                return Domain::Result<std::optional<Domain::HostSession>>::success(
                    std::nullopt);
            }
            if (existing->session.projectId != projectId) {
                return failure<std::optional<Domain::HostSession>>(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The native idempotency key belongs to another project.");
            }
            if (!existing->session.providerSessionId ||
                existing->session.status == Domain::HostSessionStatus::Cancelled) {
                return Domain::Result<std::optional<Domain::HostSession>>::success(
                    std::nullopt);
            }
            return Domain::Result<std::optional<Domain::HostSession>>::success(
                existing->session);
        } catch (...) {
            return failure<std::optional<Domain::HostSession>>(
                Domain::ErrorCodes::InternalFailure,
                "The native idempotency query failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return valid;
            }
            valid = Domain::validateBootstrapCompatibility(session, handoff);
            if (!valid) {
                return valid;
            }
            auto document = codec.encode(handoff, context);
            if (!document) {
                return propagate<void>(std::move(document));
            }
            if (document.value().canonicalUtf8.size() >
                    Domain::MaximumContinuityHandoffEncodedBytes ||
                document.value().handoff.contentSha256 !=
                    handoff.contentSha256) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The native bootstrap canonical handoff did not preserve its digest."));
            }
            if (!session.providerSessionId) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The native bootstrap session has no provider binding."));
            }
            Domain::ProviderSessionId providerId = *session.providerSessionId;
            const Domain::SessionCreationRequest binding{
                session.operationId,
                session.projectId,
                session.predecessorSessionId,
                session.idempotencyKey};
            bool durableReady{};
            {
                std::lock_guard lock{mutex};
                valid = ensureLoadedLocked(context);
                if (!valid) {
                    return valid;
                }
                auto record = findBySessionLocked(session.id);
                if (record == ledger.records.end()) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::SessionNotFound,
                        "The native bootstrap session was not found."));
                }
                valid = Domain::validateHostSessionBinding(session, binding);
                if (!valid || !record->session.providerSessionId ||
                    record->session.providerSessionId !=
                        session.providerSessionId) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The native bootstrap session does not match its durable binding."));
                }
                providerId = *record->session.providerSessionId;
                durableReady = record->session.status ==
                        Domain::HostSessionStatus::Ready &&
                    record->handoffId == handoff.handoffId &&
                    record->handoffSha256 == handoff.contentSha256;
                if (record->session.status ==
                    Domain::HostSessionStatus::Cancelled) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The native bootstrap session was cancelled."));
                }
                if (record->handoffId &&
                    (*record->handoffId != handoff.handoffId ||
                     *record->handoffSha256 != handoff.contentSha256)) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The native session is already bound to another handoff."));
                }
                if (!durableReady) {
                    record->session.status =
                        Domain::HostSessionStatus::Bootstrapping;
                    record->ownerOperationId = context.operationId;
                    record->handoffId = handoff.handoffId;
                    record->handoffSha256 = handoff.contentSha256;
                    record->updatedAt = clock.utcNow();
                    valid = commitLocked(context);
                    if (!valid) {
                        return valid;
                    }
                }
            }

            if (durableReady) {
                auto providerStatus = transport.query(providerId, context);
                if (providerStatus &&
                    providerStatus.value() == Domain::HostSessionStatus::Ready) {
                    return Domain::Result<void>::success();
                }
                if (!providerStatus && providerStatus.error().code !=
                                           Domain::ErrorCodes::SessionNotFound) {
                    return propagate<void>(std::move(providerStatus));
                }
                if (!providerStatus) {
                    auto recreated = transport.createSession(binding, context);
                    if (!recreated) {
                        return propagate<void>(std::move(recreated));
                    }
                    valid = validateProvider(recreated.value());
                    if (!valid || recreated.value().providerSessionId !=
                                      providerId) {
                        return Domain::Result<void>::failure(Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The recovered provider session differs from its durable binding."));
                    }
                }
                std::lock_guard lock{mutex};
                auto record = findBySessionLocked(session.id);
                if (record == ledger.records.end() ||
                    record->handoffId != handoff.handoffId ||
                    record->handoffSha256 != handoff.contentSha256) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The recovered bootstrap binding changed before replay."));
                }
                record->session.status =
                    Domain::HostSessionStatus::Bootstrapping;
                record->ownerOperationId = context.operationId;
                record->updatedAt = clock.utcNow();
                valid = commitLocked(context);
                if (!valid) {
                    return valid;
                }
            }

            auto response = transport.bootstrap(
                Domain::NativeBootstrapRequest{
                    session.operationId,
                    session.projectId,
                    session.id,
                    providerId,
                    handoff.handoffId,
                    handoff.contentSha256,
                    document.value().canonicalUtf8},
                context);
            if (!response) {
                return propagate<void>(std::move(response));
            }
            valid = validateAcknowledgement(
                response.value(), handoff.handoffId, session.id);
            if (!valid) {
                return valid;
            }

            std::lock_guard lock{mutex};
            auto record = findBySessionLocked(session.id);
            if (record == ledger.records.end() ||
                record->handoffId != handoff.handoffId ||
                record->handoffSha256 != handoff.contentSha256) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The native bootstrap intent changed before acknowledgement."));
            }
            if (record->session.status == Domain::HostSessionStatus::Cancelled ||
                cancelledOperations.contains(context.operationId.value()) ||
                context.isCancellationRequested()) {
                transport.cancel(
                    context.operationId,
                    record->session.providerSessionId);
                record->session.status = Domain::HostSessionStatus::Cancelled;
                record->updatedAt = clock.utcNow();
                static_cast<void>(commitLocked(context));
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The native bootstrap was cancelled after provider acknowledgement."));
            }
            record->session.status = Domain::HostSessionStatus::Ready;
            record->inputTokens =
                static_cast<std::uint64_t>(response.value().inputTokens);
            record->outputTokens =
                static_cast<std::uint64_t>(response.value().outputTokens);
            record->updatedAt = clock.utcNow();
            return commitLocked(context);
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The native session-host bootstrap failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::HandoffAcknowledgement> acknowledge(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<Domain::HandoffAcknowledgement>(
                    std::move(valid));
            }
            std::lock_guard lock{mutex};
            valid = ensureLoadedLocked(context);
            if (!valid) {
                return propagate<Domain::HandoffAcknowledgement>(
                    std::move(valid));
            }
            const auto record = findBySessionLocked(session.id);
            if (record == ledger.records.end() ||
                record->session.status != Domain::HostSessionStatus::Ready ||
                record->handoffId != handoffId ||
                record->handoffSha256 != handoffSha256 ||
                record->session.projectId != session.projectId ||
                record->session.operationId != session.operationId) {
                return failure<Domain::HandoffAcknowledgement>(
                    Domain::ErrorCodes::AcknowledgementTimeout,
                    "The exact native handoff acknowledgement is not ready.",
                    true);
            }
            return Domain::Result<Domain::HandoffAcknowledgement>::success(
                Domain::HandoffAcknowledgement{
                    handoffId,
                    session.id,
                    adapterId,
                    handoffSha256});
        } catch (...) {
            return failure<Domain::HandoffAcknowledgement>(
                Domain::ErrorCodes::InternalFailure,
                "The native acknowledgement wait failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<Domain::HostSessionStatus>(std::move(valid));
            }
            std::lock_guard lock{mutex};
            valid = ensureLoadedLocked(context);
            if (!valid) {
                return propagate<Domain::HostSessionStatus>(std::move(valid));
            }
            const auto record = findBySessionLocked(sessionId);
            if (record == ledger.records.end()) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::SessionNotFound,
                    "The native logical session was not found.");
            }
            return Domain::Result<Domain::HostSessionStatus>::success(
                record->session.status);
        } catch (...) {
            return failure<Domain::HostSessionStatus>(
                Domain::ErrorCodes::InternalFailure,
                "The native logical-session query failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<Domain::HostRecoveryReport>(std::move(valid));
            }
            std::vector<Domain::NativeSessionRecord> selected;
            {
                std::lock_guard lock{mutex};
                valid = ensureLoadedLocked(context);
                if (!valid) {
                    return propagate<Domain::HostRecoveryReport>(
                        std::move(valid));
                }
                for (const auto& record : ledger.records) {
                    if ((!request.projectId ||
                         record.session.projectId == *request.projectId) &&
                        (!request.operationId ||
                         record.session.operationId == *request.operationId)) {
                        selected.push_back(record);
                    }
                }
            }
            Domain::HostRecoveryReport report{};
            report.sessions.reserve(selected.size());
            for (const auto& record : selected) {
                ++report.inspected;
                if (record.session.status ==
                    Domain::HostSessionStatus::Cancelled) {
                    ++report.cancelled;
                    report.sessions.push_back(record.session);
                    continue;
                }
                if (!record.session.providerSessionId) {
                    const Domain::SessionCreationRequest creation{
                        record.session.operationId,
                        record.session.projectId,
                        record.session.predecessorSessionId,
                        record.session.idempotencyKey};
                    auto restored = create(creation, context);
                    if (!restored) {
                        ++report.failed;
                        continue;
                    }
                    ++report.recovered;
                    report.sessions.push_back(std::move(restored).value());
                    continue;
                }
                auto providerStatus = transport.query(
                    *record.session.providerSessionId, context);
                if (!providerStatus) {
                    ++report.failed;
                    continue;
                }
                ++report.recovered;
                report.sessions.push_back(record.session);
            }
            return Domain::Result<Domain::HostRecoveryReport>::success(
                std::move(report));
        } catch (...) {
            return failure<Domain::HostRecoveryReport>(
                Domain::ErrorCodes::InternalFailure,
                "The native session-host recovery failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeSessionHostHealth> health(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock, shutdownRequested);
            if (!valid) {
                return propagate<Domain::NativeSessionHostHealth>(
                    std::move(valid));
            }
            std::lock_guard lock{mutex};
            valid = ensureLoadedLocked(context);
            if (!valid) {
                return propagate<Domain::NativeSessionHostHealth>(
                    std::move(valid));
            }
            return Domain::Result<Domain::NativeSessionHostHealth>::success(
                Domain::NativeSessionHostHealth{
                    true,
                    ledger.records.size(),
                    Domain::MaximumNativeSessionRecords,
                    Domain::MaximumNativeResponseBytes});
        } catch (...) {
            return failure<Domain::NativeSessionHostHealth>(
                Domain::ErrorCodes::InternalFailure,
                "The native session-host health query failed safely.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::vector<std::optional<Domain::ProviderSessionId>> providers;
            {
                std::lock_guard lock{mutex};
                if (cancelledOperations.size() >= 256U &&
                    !cancelledOrder.empty()) {
                    cancelledOperations.erase(cancelledOrder.front());
                    cancelledOrder.pop_front();
                }
                if (cancelledOperations.insert(operationId.value()).second) {
                    cancelledOrder.push_back(operationId.value());
                }
                const Domain::OperationContext context{
                    operationId,
                    clock.monotonicNow() + std::chrono::seconds{10},
                    {},
                    cancelCorrelationId()};
                if (ensureLoadedLocked(context)) {
                    for (auto& record : ledger.records) {
                        if (record.ownerOperationId == operationId) {
                            record.session.status =
                                Domain::HostSessionStatus::Cancelled;
                            record.updatedAt = clock.utcNow();
                            providers.push_back(
                                record.session.providerSessionId);
                        }
                    }
                    if (!providers.empty()) {
                        static_cast<void>(commitLocked(context));
                    }
                }
            }
            if (providers.empty()) {
                transport.cancel(operationId, std::nullopt);
            } else {
                for (const auto& provider : providers) {
                    transport.cancel(operationId, provider);
                }
            }
        } catch (...) {
            transport.cancel(operationId, std::nullopt);
        }
    }

    Domain::AdapterId adapterId;
    Contracts::INativeSessionLedger& ledgerStore;
    Contracts::INativeSessionTransport& transport;
    Contracts::IContinuityDocumentCodec& codec;
    Contracts::IUuidGenerator& uuidGenerator;
    Contracts::IClock& clock;
    std::mutex mutex;
    Domain::NativeSessionLedger ledger;
    std::unordered_set<std::string> cancelledOperations;
    std::deque<std::string> cancelledOrder;
    std::atomic_bool shutdownRequested{};
    bool loaded{};
};

ForgeNativeSessionHostAdapter::ForgeNativeSessionHostAdapter(
    Domain::AdapterId adapterId,
    Contracts::INativeSessionLedger& ledger,
    Contracts::INativeSessionTransport& transport,
    Contracts::IContinuityDocumentCodec& codec,
    Contracts::IUuidGenerator& uuidGenerator,
    Contracts::IClock& clock)
    : implementation_{std::make_unique<Impl>(
          std::move(adapterId),
          ledger,
          transport,
          codec,
          uuidGenerator,
          clock)}
{
}

ForgeNativeSessionHostAdapter::~ForgeNativeSessionHostAdapter() noexcept
{
    shutdown();
}

const Domain::AdapterId&
ForgeNativeSessionHostAdapter::identifier() const noexcept
{
    return implementation_->adapterId;
}

std::string_view ForgeNativeSessionHostAdapter::version() const noexcept
{
    return AdapterVersion;
}

Domain::Result<Domain::HostCapabilities>
ForgeNativeSessionHostAdapter::capabilities(
    const Domain::OperationContext& context) noexcept
{
    auto valid = validateContext(
        context, implementation_->clock, implementation_->shutdownRequested);
    if (!valid) {
        return propagate<Domain::HostCapabilities>(std::move(valid));
    }
    return Domain::Result<Domain::HostCapabilities>::success(
        Domain::HostCapabilities{
            true, true, true, true, true, true, true, true});
}

Domain::Result<Domain::HostSession>
ForgeNativeSessionHostAdapter::createSession(
    const Domain::SessionCreationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->create(request, context);
}

Domain::Result<std::optional<Domain::HostSession>>
ForgeNativeSessionHostAdapter::queryByIdempotencyKey(
    const Domain::ProjectId& projectId,
    const Domain::IdempotencyKey& key,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->byKey(projectId, key, context);
}

Domain::Result<void> ForgeNativeSessionHostAdapter::bootstrap(
    const Domain::HostSession& session,
    const Domain::ContinuityHandoff& handoff,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->bootstrap(session, handoff, context);
}

Domain::Result<Domain::HandoffAcknowledgement>
ForgeNativeSessionHostAdapter::awaitAcknowledgement(
    const Domain::HostSession& session,
    const Domain::ContinuityHandoffId& handoffId,
    const Domain::Sha256Digest& handoffSha256,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->acknowledge(
        session, handoffId, handoffSha256, context);
}

Domain::Result<Domain::HostSessionStatus>
ForgeNativeSessionHostAdapter::query(
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->query(sessionId, context);
}

Domain::Result<Domain::HostRecoveryReport>
ForgeNativeSessionHostAdapter::recover(
    const Domain::HostRecoveryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->recover(request, context);
}

Domain::Result<Domain::NativeSessionHostHealth>
ForgeNativeSessionHostAdapter::health(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->health(context);
}

void ForgeNativeSessionHostAdapter::cancel(
    const Domain::OperationId& operationId) noexcept
{
    implementation_->cancel(operationId);
}

void ForgeNativeSessionHostAdapter::shutdown() noexcept
{
    if (implementation_ &&
        !implementation_->shutdownRequested.exchange(
            true, std::memory_order_acq_rel)) {
        implementation_->transport.shutdown();
        implementation_->ledgerStore.shutdown();
    }
}

Domain::Result<Domain::HostPluginManifest>
nativeSessionHostPluginManifest(const Domain::PathText& executable)
{
    auto adapterId = Domain::AdapterId::parse(
        ForgeNativeSessionHostAdapter::AdapterIdentifier);
    if (!adapterId) {
        return propagate<Domain::HostPluginManifest>(std::move(adapterId));
    }
    return Domain::Result<Domain::HostPluginManifest>::success(
        Domain::HostPluginManifest{
            std::move(adapterId).value(),
            std::string{ForgeNativeSessionHostAdapter::AdapterVersion},
            ForgeNativeSessionHostAdapter::ProtocolVersion,
            executable,
            Domain::HostCapabilities{
                true, true, true, true, true, true, true, true}});
}

} // namespace ForgeConductor::SessionHost
