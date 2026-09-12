#include "ForgeConductor/Application/ManagedRunService.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Contracts = ForgeConductor::Contracts;
namespace Application = ForgeConductor::Application;

template <typename T>
[[nodiscard]] T parsed(Domain::Result<T> value)
{
    assert(value);
    return std::move(value).value();
}

class Clock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return {};
    }

    void advance()
    {
        const std::lock_guard lock{mutex_};
        utc_ += 1s;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_{};
};

class Store final : public Contracts::IManagedRunStore {
public:
    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        const std::lock_guard lock{mutex_};
        const auto found = records_.find(runId);
        return Domain::Result<
            std::optional<Domain::ManagedRunRecord>>::success(
            found == records_.end()
                ? std::nullopt
                : std::optional<Domain::ManagedRunRecord>{found->second});
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext&) noexcept override
    {
        const std::lock_guard lock{mutex_};
        records_.insert_or_assign(record.runId, record);
        ++saves;
        return Domain::Result<void>::success();
    }

    std::size_t saves{};

private:
    std::mutex mutex_;
    std::map<Domain::SessionId, Domain::ManagedRunRecord> records_;
};

class Transport final : public Contracts::IManagedResponsesTransport {
public:
    enum class Mode { Success, Offline, Block };

    [[nodiscard]] Domain::Result<Domain::ManagedProviderTurnResult> complete(
        const Domain::ManagedProviderTurnRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++calls;
            lastProject = request.projectId.value();
            lastRun = request.runId.value();
            lastGeneration = request.authorityGeneration;
        }
        if (mode == Mode::Block) {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, context.cancellation, [this] { return released_; });
            return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The controlled provider request was cancelled."));
        }
        if (mode == Mode::Offline) {
            return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The controlled provider is offline.",
                    true));
        }
        return Domain::Result<Domain::ManagedProviderTurnResult>::success(
            Domain::ManagedProviderTurnResult{
                parsed(Domain::ProviderSessionId::parse("resp_ordinary_1")),
                "ordinary run completed",
                21U,
                8U,
                29U});
    }

    void cancel(
        const Domain::OperationId&,
        const std::optional<Domain::ProviderSessionId>&) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++cancels;
        }
        cv_.notify_all();
    }

    Mode mode{Mode::Success};
    std::size_t calls{};
    std::size_t cancels{};
    std::string lastProject;
    std::string lastRun;
    std::uint64_t lastGeneration{};

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool released_{};
};

[[nodiscard]] Domain::OperationContext context(
    const char* operation,
    const char* correlation)
{
    return Domain::OperationContext{
        parsed(Domain::OperationId::parse(operation)),
        Domain::MonotonicTimePoint::max(),
        {},
        parsed(Domain::CorrelationId::parse(correlation))};
}

[[nodiscard]] Domain::ManagedRunStartRequest request(
    const char* run,
    const char* operation,
    const char* task)
{
    return Domain::ManagedRunStartRequest{
        parsed(Domain::SessionId::parse(run)),
        parsed(Domain::ProjectId::parse(
            "11111111-1111-4111-8111-111111111111")),
        parsed(Domain::ClientId::parse("manager-owned-test")),
        parsed(Domain::OperationId::parse(operation)),
        parsed(Domain::CorrelationId::parse("managed-run-test")),
        7U,
        task};
}

[[nodiscard]] Domain::ManagedRunSnapshot waitForTerminal(
    Application::ManagedRunService& service,
    const Domain::SessionId& runId)
{
    const auto query = context(
        "99999999-9999-4999-8999-999999999999",
        "managed-run-status");
    for (std::size_t attempt = 0; attempt < 200U; ++attempt) {
        auto current = service.status(runId, query);
        assert(current);
        if (current.value().record.state != Domain::ManagedRunState::Running &&
            current.value().record.state !=
                Domain::ManagedRunState::Cancelling) {
            return std::move(current).value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false);
    return service.status(runId, query).value();
}

} // namespace

int main()
{
    Clock clock;
    Store store;
    Transport transport;
    Application::ManagedRunService service{transport, store, clock};

    const auto first = request(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        "Perform ordinary work.");
    const auto startContext = context(
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        "managed-run-test");
    auto started = service.start(first, startContext);
    assert(started);
    assert(started.value().managerOwned);
    assert(started.value().record.runId == first.runId);
    assert(started.value().record.projectId == first.projectId);

    auto completed = waitForTerminal(service, first.runId);
    assert(completed.record.state == Domain::ManagedRunState::Completed);
    assert(completed.record.providerResponseId);
    assert(completed.record.providerResponseId->value() == "resp_ordinary_1");
    assert(completed.record.inputTokens == 21U);
    assert(completed.record.outputTokens == 8U);
    assert(completed.record.retainedContextTokens == 29U);
    assert(completed.record.outputText == "ordinary run completed");
    assert(transport.calls == 1U);
    assert(transport.lastProject == first.projectId.value());
    assert(transport.lastRun == first.runId.value());
    assert(transport.lastGeneration == 7U);

    auto duplicate = service.start(first, startContext);
    assert(duplicate);
    assert(transport.calls == 1U);
    auto conflicting = first;
    conflicting.task = "A different task.";
    auto conflict = service.start(conflicting, startContext);
    assert(!conflict);
    assert(conflict.error().code == Domain::ErrorCodes::Conflict);

    transport.mode = Transport::Mode::Offline;
    const auto offline = request(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
        "Observe an offline failure.");
    auto offlineStarted = service.start(
        offline,
        context(
            "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
            "managed-run-test"));
    assert(offlineStarted);
    auto failed = waitForTerminal(service, offline.runId);
    assert(failed.record.state == Domain::ManagedRunState::Failed);
    assert(failed.record.lastError);
    assert(failed.record.lastError->code ==
           Domain::ErrorCodes::TransportClosed);

    transport.mode = Transport::Mode::Block;
    const auto blocked = request(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "cccccccc-dddd-4eee-8fff-aaaaaaaaaaaa",
        "Wait until cancelled.");
    auto blockedStarted = service.start(
        blocked,
        context(
            "cccccccc-dddd-4eee-8fff-aaaaaaaaaaaa",
            "managed-run-test"));
    assert(blockedStarted);
    auto cancellation = service.cancel(
        blocked.runId,
        context(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "managed-run-cancel"));
    assert(cancellation);
    assert(cancellation.value().cancellationRequested);
    auto cancelled = waitForTerminal(service, blocked.runId);
    assert(cancelled.record.state == Domain::ManagedRunState::Cancelled);
    assert(transport.cancels >= 1U);
    assert(store.saves >= 6U);

    service.shutdown();
    return 0;
}
