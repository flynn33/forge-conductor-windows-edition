#include "ForgeConductor/Application/ManagedRunService.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::Error failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::makeError(code, message, retryable);
}

[[nodiscard]] Domain::Result<void> validate(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::Cancelled,
            "The managed run start was cancelled."));
    }
    if (request.authorityGeneration == 0U) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run requires a nonzero project authority generation."));
    }
    if (request.task.empty() ||
        request.task.size() > Domain::MaximumManagedRunTaskBytes ||
        request.task.find('\0') != std::string::npos ||
        !Domain::isValidUtf8(request.task)) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run task is empty, invalid, or exceeds its bound."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::ManagedRunSnapshot snapshot(
    const Domain::ManagedRunRecord& record,
    const bool cancellationRequested)
{
    return Domain::ManagedRunSnapshot{
        record,
        true,
        cancellationRequested};
}

} // namespace

class ManagedRunService::Impl final {
public:
    Impl(
        Contracts::IManagedResponsesTransport& transport,
        Contracts::IManagedRunStore& store,
        Contracts::IClock& clock)
        : transport_{transport}, store_{store}, clock_{clock}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (auto valid = validate(request, context); !valid) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(valid).error());
            }
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::TransportClosed,
                            "The managed run service is shut down."));
                }
                const auto found = active_.find(request.runId);
                if (found != active_.end()) {
                    if (sameRequest(found->second->request, request)) {
                        return Domain::Result<Domain::ManagedRunSnapshot>::success(
                            snapshot(
                                found->second->record,
                                found->second->worker.get_stop_token()
                                    .stop_requested()));
                    }
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id is already bound to another request."));
                }
            }

            auto persisted = store_.load(request.runId, context);
            if (!persisted) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(persisted).error());
            }
            if (persisted.value()) {
                if (persisted.value()->projectId == request.projectId &&
                    persisted.value()->clientId == request.clientId &&
                    persisted.value()->task == request.task) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(*persisted.value(), false));
                }
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    failure(
                        Domain::ErrorCodes::Conflict,
                        "The durable managed run id belongs to another request."));
            }

            const auto now = clock_.utcNow();
            Domain::ManagedRunRecord record{
                request.runId,
                request.projectId,
                request.clientId,
                request.task,
                Domain::ManagedRunState::Running,
                std::nullopt,
                0U,
                0U,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                now,
                now};
            if (auto saved = store_.save(record, context); !saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
            }

            auto active = std::make_shared<ActiveRun>(request, record);
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::TransportClosed,
                            "The managed run service shut down during admission."));
                }
                const auto [_, inserted] =
                    active_.emplace(request.runId, active);
                if (!inserted) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id was admitted concurrently."));
                }
                active->worker = std::jthread{
                    [this, request](const std::stop_token token) {
                        execute(request, token);
                    }};
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(record, false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run could not be started safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(
                            found->second->record,
                            found->second->worker.get_stop_token()
                                .stop_requested()));
                }
            }
            auto persisted = store_.load(runId, context);
            if (!persisted) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(persisted).error());
            }
            if (!persisted.value()) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    failure(
                        Domain::ErrorCodes::SessionNotFound,
                        "The managed run was not found."));
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(*persisted.value(), false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run status failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            std::optional<Domain::ProviderSessionId> responseId;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    active = found->second;
                    if (active->record.state ==
                        Domain::ManagedRunState::Running) {
                        active->record.state =
                            Domain::ManagedRunState::Cancelling;
                        active->record.updatedAt = clock_.utcNow();
                    }
                    active->worker.request_stop();
                    responseId = active->record.providerResponseId;
                }
            }
            if (!active) {
                return status(runId, context);
            }
            transport_.cancel(
                active->request.operationId,
                responseId);
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(active->record, true));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run cancellation failed safely."));
        }
    }

    void shutdown() noexcept
    {
        std::vector<std::shared_ptr<ActiveRun>> active;
        try {
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return;
                }
                shutdown_ = true;
                active.reserve(active_.size());
                for (const auto& entry : active_) {
                    active.push_back(entry.second);
                }
            }
            for (const auto& run : active) {
                run->worker.request_stop();
                transport_.cancel(
                    run->request.operationId,
                    std::nullopt);
            }
            for (const auto& run : active) {
                if (run->worker.joinable()) {
                    run->worker.join();
                }
            }
        } catch (...) {
        }
    }

private:
    struct ActiveRun final {
        ActiveRun(
            Domain::ManagedRunStartRequest value,
            Domain::ManagedRunRecord initial)
            : request{std::move(value)}, record{std::move(initial)}
        {
        }

        Domain::ManagedRunStartRequest request;
        Domain::ManagedRunRecord record;
        std::jthread worker;
    };

    [[nodiscard]] static bool sameRequest(
        const Domain::ManagedRunStartRequest& left,
        const Domain::ManagedRunStartRequest& right) noexcept
    {
        return left.runId == right.runId &&
            left.projectId == right.projectId &&
            left.clientId == right.clientId &&
            left.operationId == right.operationId &&
            left.authorityGeneration == right.authorityGeneration &&
            left.task == right.task;
    }

    void execute(
        const Domain::ManagedRunStartRequest& request,
        const std::stop_token token) noexcept
    {
        const Domain::OperationContext providerContext{
            request.operationId,
            Domain::MonotonicTimePoint::max(),
            token,
            request.correlationId};
        Domain::ManagedProviderTurnRequest turn{
            request.projectId,
            request.runId,
            request.authorityGeneration,
            request.task,
            std::nullopt};
        auto outcome = transport_.complete(turn, providerContext);

        Domain::ManagedRunRecord record = [&] {
            std::lock_guard lock{mutex_};
            return active_.at(request.runId)->record;
        }();
        record.updatedAt = clock_.utcNow();
        if (outcome) {
            auto value = std::move(outcome).value();
            record.providerResponseId = std::move(value.responseId);
            record.inputTokens = value.inputTokens;
            record.outputTokens = value.outputTokens;
            record.retainedContextTokens = value.retainedContextTokens;
            if (value.outputText.size() > Domain::MaximumManagedRunOutputBytes) {
                value.outputText.resize(Domain::MaximumManagedRunOutputBytes);
            }
            record.outputText = std::move(value.outputText);
            record.state = token.stop_requested()
                ? Domain::ManagedRunState::Cancelled
                : Domain::ManagedRunState::Completed;
        } else {
            record.lastError = outcome.error();
            record.state =
                token.stop_requested() ||
                    outcome.error().code == Domain::ErrorCodes::Cancelled
                ? Domain::ManagedRunState::Cancelled
                : Domain::ManagedRunState::Failed;
        }

        const Domain::OperationContext persistenceContext{
            request.operationId,
            Domain::MonotonicTimePoint::max(),
            {},
            request.correlationId};
        if (auto saved = store_.save(record, persistenceContext); !saved) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = saved.error();
        }
        try {
            std::lock_guard lock{mutex_};
            const auto found = active_.find(request.runId);
            if (found != active_.end()) {
                found->second->record = std::move(record);
            }
        } catch (...) {
        }
    }

    Contracts::IManagedResponsesTransport& transport_;
    Contracts::IManagedRunStore& store_;
    Contracts::IClock& clock_;
    std::mutex mutex_;
    std::map<Domain::SessionId, std::shared_ptr<ActiveRun>> active_;
    bool shutdown_{};
};

ManagedRunService::ManagedRunService(
    Contracts::IManagedResponsesTransport& transport,
    Contracts::IManagedRunStore& store,
    Contracts::IClock& clock)
    : implementation_{
          std::make_unique<Impl>(transport, store, clock)}
{
}

ManagedRunService::~ManagedRunService() noexcept = default;

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::start(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->start(request, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::status(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::cancel(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->cancel(runId, context);
}

void ManagedRunService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
