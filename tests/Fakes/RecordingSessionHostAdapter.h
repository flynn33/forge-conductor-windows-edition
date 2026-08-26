#pragma once

#include "ForgeConductor/Contracts/ISessionHostAdapter.h"
#include "DeterministicResult.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

enum class SessionHostCall : std::size_t {
    Capabilities,
    CreateSession,
    QueryByIdempotencyKey,
    Bootstrap,
    AwaitAcknowledgement,
    Query,
    Recover,
    Count
};

class RecordingSessionHostAdapter final
    : public Contracts::ISessionHostAdapter {
public:
    RecordingSessionHostAdapter(
        Domain::AdapterId adapterId,
        std::string version)
        : adapterId_{std::move(adapterId)},
          version_{std::move(version)}
    {
    }

    DeterministicResult<Domain::HostCapabilities> capabilitiesResult;
    DeterministicResult<Domain::HostSession> createSessionResult;
    DeterministicResult<std::optional<Domain::HostSession>>
        queryByIdempotencyKeyResult;
    DeterministicResult<void> bootstrapResult;
    DeterministicResult<Domain::HandoffAcknowledgement>
        acknowledgementResult;
    DeterministicResult<Domain::HostSessionStatus> queryResult;
    DeterministicResult<Domain::HostRecoveryReport> recoverResult;

    [[nodiscard]] const Domain::AdapterId&
    identifier() const noexcept override
    {
        return adapterId_;
    }

    [[nodiscard]] std::string_view version() const noexcept override
    {
        return version_;
    }

    [[nodiscard]] Domain::Result<Domain::HostCapabilities> capabilities(
        const Domain::OperationContext& context) noexcept override
    {
        return finish(
            SessionHostCall::Capabilities,
            context,
            capabilitiesResult);
    }

    [[nodiscard]] Domain::Result<Domain::HostSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCreationRequest_ = request;
            return finish(
                SessionHostCall::CreateSession,
                context,
                createSessionResult);
        } catch (...) {
            return recordingFailure<Domain::HostSession>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::HostSession>>
    queryByIdempotencyKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastIdempotencyProject_ = projectId;
            lastIdempotencyKey_ = key;
            return finish(
                SessionHostCall::QueryByIdempotencyKey,
                context,
                queryByIdempotencyKeyResult);
        } catch (...) {
            return recordingFailure<std::optional<Domain::HostSession>>();
        }
    }

    [[nodiscard]] Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto compatible =
                Domain::validateBootstrapCompatibility(session, handoff);
            if (!compatible) {
                return Domain::Result<void>::failure(
                    std::move(compatible).error());
            }
            auto result = finish(
                SessionHostCall::Bootstrap,
                context,
                bootstrapResult);
            if (!result) {
                return result;
            }
            lastBootstrapSession_.emplace(session);
            lastBootstrapSessionId_ = session.id;
            lastBootstrapHandoffId_ = handoff.handoffId;
            lastBootstrapSha256_ = handoff.contentSha256;
            return result;
        } catch (...) {
            return recordingFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HandoffAcknowledgement>
    awaitAcknowledgement(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            if (!lastBootstrapSession_ || !lastBootstrapHandoffId_ ||
                !lastBootstrapSha256_ ||
                !sameSessionBinding(lastBootstrapSession_.value(), session) ||
                lastBootstrapHandoffId_.value() != handoffId ||
                lastBootstrapSha256_.value() != handoffSha256) {
                return Domain::Result<Domain::HandoffAcknowledgement>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Acknowledgement requires the exact prior bootstrap session and handoff."));
            }
            lastAcknowledgementSessionId_ = session.id;
            lastAcknowledgementHandoffId_ = handoffId;
            lastAcknowledgementSha256_ = handoffSha256;
            auto result = finish(
                SessionHostCall::AwaitAcknowledgement,
                context,
                acknowledgementResult);
            if (!result) {
                return result;
            }
            const auto& acknowledgement = result.value();
            if (acknowledgement.handoffId != handoffId ||
                acknowledgement.successorSessionId != session.id ||
                acknowledgement.adapterId != adapterId_ ||
                acknowledgement.canonicalHandoffSha256 != handoffSha256) {
                return Domain::Result<Domain::HandoffAcknowledgement>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The host acknowledgement did not match the exact handoff evidence."));
            }
            return result;
        } catch (...) {
            return recordingFailure<Domain::HandoffAcknowledgement>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastQueriedSessionId_ = sessionId;
            return finish(
                SessionHostCall::Query,
                context,
                queryResult);
        } catch (...) {
            return recordingFailure<Domain::HostSessionStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastRecoveryRequest_ = request;
            return finish(
                SessionHostCall::Recover,
                context,
                recoverResult);
        } catch (...) {
            return recordingFailure<Domain::HostRecoveryReport>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            lastCancelledOperation_ = operationId;
            ++cancelCalls_;
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] std::size_t callCount(
        const SessionHostCall call) const noexcept
    {
        return calls_[static_cast<std::size_t>(call)];
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        return cancelCalls_;
    }

    [[nodiscard]] const std::optional<Domain::SessionCreationRequest>&
    lastCreationRequest() const noexcept
    {
        return lastCreationRequest_;
    }

    [[nodiscard]] const std::optional<Domain::ProjectId>&
    lastIdempotencyProject() const noexcept
    {
        return lastIdempotencyProject_;
    }

    [[nodiscard]] const std::optional<Domain::IdempotencyKey>&
    lastIdempotencyKey() const noexcept
    {
        return lastIdempotencyKey_;
    }

    [[nodiscard]] const std::optional<Domain::SessionId>&
    lastAcknowledgementSessionId() const noexcept
    {
        return lastAcknowledgementSessionId_;
    }

    [[nodiscard]] const std::optional<Domain::ContinuityHandoffId>&
    lastAcknowledgementHandoffId() const noexcept
    {
        return lastAcknowledgementHandoffId_;
    }

    [[nodiscard]] const std::optional<Domain::Sha256Digest>&
    lastAcknowledgementSha256() const noexcept
    {
        return lastAcknowledgementSha256_;
    }

    [[nodiscard]] const std::optional<Domain::SessionId>&
    lastQueriedSessionId() const noexcept
    {
        return lastQueriedSessionId_;
    }

    [[nodiscard]] const std::optional<Domain::HostRecoveryRequest>&
    lastRecoveryRequest() const noexcept
    {
        return lastRecoveryRequest_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastCancelledOperation() const noexcept
    {
        return lastCancelledOperation_;
    }

private:
    template <typename T>
    [[nodiscard]] Domain::Result<T> finish(
        const SessionHostCall call,
        const Domain::OperationContext& context,
        const DeterministicResult<T>& result) noexcept
    {
        try {
            ++calls_[static_cast<std::size_t>(call)];
            lastOperationId_ = context.operationId;
            if (shutdown_ || context.isCancellationRequested() ||
                (lastCancelledOperation_ &&
                 lastCancelledOperation_.value() == context.operationId)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic session-host operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic session-host deadline expired."));
            }
            return result.get();
        } catch (...) {
            return recordingFailure<T>();
        }
    }

    [[nodiscard]] static bool sameSessionBinding(
        const Domain::HostSession& left,
        const Domain::HostSession& right) noexcept
    {
        return left.id == right.id &&
            left.projectId == right.projectId &&
            left.operationId == right.operationId &&
            left.predecessorSessionId == right.predecessorSessionId &&
            left.idempotencyKey == right.idempotencyKey &&
            left.providerSessionId == right.providerSessionId &&
            left.model == right.model &&
            left.status == right.status;
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> recordingFailure() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic session-host call could not be recorded."));
    }

    Domain::AdapterId adapterId_;
    std::string version_;
    std::array<
        std::size_t,
        static_cast<std::size_t>(SessionHostCall::Count)> calls_{};
    std::optional<Domain::SessionCreationRequest> lastCreationRequest_;
    std::optional<Domain::ProjectId> lastIdempotencyProject_;
    std::optional<Domain::IdempotencyKey> lastIdempotencyKey_;
    std::optional<Domain::HostSession> lastBootstrapSession_;
    std::optional<Domain::SessionId> lastBootstrapSessionId_;
    std::optional<Domain::ContinuityHandoffId> lastBootstrapHandoffId_;
    std::optional<Domain::Sha256Digest> lastBootstrapSha256_;
    std::optional<Domain::SessionId> lastAcknowledgementSessionId_;
    std::optional<Domain::ContinuityHandoffId> lastAcknowledgementHandoffId_;
    std::optional<Domain::Sha256Digest> lastAcknowledgementSha256_;
    std::optional<Domain::SessionId> lastQueriedSessionId_;
    std::optional<Domain::HostRecoveryRequest> lastRecoveryRequest_;
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Domain::OperationId> lastCancelledOperation_;
    Domain::MonotonicTimePoint now_{};
    std::size_t cancelCalls_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
