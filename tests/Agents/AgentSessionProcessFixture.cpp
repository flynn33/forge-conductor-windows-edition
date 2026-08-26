#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

using namespace std::chrono_literals;

enum class ExitCode : int {
    Success = 0,
    Usage = 2,
    OpenFailed = 10,
    EventFailed = 11,
    OperationFailed = 12,
    OwnershipConflict = 41,
    UnexpectedFailure = 90
};

[[nodiscard]] int exitCode(const ExitCode value) noexcept
{
    return static_cast<int>(value);
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    return take(InfrastructureDetail::strictUtf16ToUtf8(value));
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::wstring_view value)
{
    return take(Identifier::parse(utf8(value)));
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Contracts::IClock& clock,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse(
            "a1000000-0000-4000-8000-000000000001")),
        clock.monotonicNow() + 60s,
        {},
        take(Domain::CorrelationId::parse(correlation))};
}

struct RepositoryContext final {
    std::shared_ptr<InfrastructureWindows::SystemClock> clock{
        std::make_shared<InfrastructureWindows::SystemClock>()};
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths{
        std::make_shared<Fakes::RecordingApplicationPathsFake>()};
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics{
        std::make_shared<Fakes::RuntimeDiagnosticsFake>(clock->monotonicNow())};

    explicit RepositoryContext(const std::filesystem::path& directory)
    {
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(
            Domain::Result<Domain::PathText>::success(
                Support::pathText(directory)));
    }
};

class BlockingTransactionObserver final
    : public PersistenceWindows::IAgentSessionTransactionObserver {
public:
    BlockingTransactionObserver(
        const std::wstring_view readyEvent,
        const std::wstring_view releaseEvent,
        const PersistenceWindows::AgentSessionTransactionKind kind,
        const PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint)
        : ready_{::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEvent.data())},
          release_{::OpenEventW(SYNCHRONIZE, FALSE, releaseEvent.data())},
          kind_{kind}, checkpoint_{checkpoint}
    {
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return static_cast<bool>(ready_) && static_cast<bool>(release_);
    }

    void onAgentSessionTransactionCheckpoint(
        const PersistenceWindows::AgentSessionTransactionKind kind,
        const PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint)
        noexcept override
    {
        if (kind != kind_ ||
            checkpoint != checkpoint_) {
            return;
        }
        if (::SetEvent(ready_.get()) != FALSE) {
            static_cast<void>(::WaitForSingleObject(release_.get(), 60'000U));
        }
    }

private:
    InfrastructureDetail::UniqueHandle ready_;
    InfrastructureDetail::UniqueHandle release_;
    PersistenceWindows::AgentSessionTransactionKind kind_;
    PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint_;
};

[[nodiscard]] Domain::AgentRunStartMutation startMutation(
    const Domain::SessionId& sessionId,
    const Domain::ClientId& clientId,
    const Domain::UtcTimePoint now)
{
    const auto agentId = take(Domain::AgentId::parse("implement"));
    Domain::AgentRunRecord run{
        Domain::AgentSession{
            sessionId,
            agentId,
            clientId,
            Domain::SessionStatus::Open,
            std::nullopt,
            now,
            now},
        std::nullopt,
        std::string{"crash-atomic-goal"},
        std::nullopt,
        {"result"},
        {"inspect"},
        std::nullopt};
    Domain::ActiveBinding binding{
        sessionId,
        agentId,
        "crash-atomic-goal",
        {"read"},
        {},
        {"result"},
        {"persist"},
        std::nullopt};
    return Domain::AgentRunStartMutation{
        std::move(run),
        std::move(binding),
        Domain::makeAgentSupersedeSummary(
            "Closed because a process-fixture run started",
            std::optional<Domain::AgentId>{agentId},
            std::nullopt)};
}

[[nodiscard]] int crashDuringStart(
    wchar_t** const arguments,
    const PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint)
{
    RepositoryContext fixture{std::filesystem::path{arguments[2]}};
    BlockingTransactionObserver observer{
        arguments[4],
        arguments[5],
        PersistenceWindows::AgentSessionTransactionKind::Start,
        checkpoint};
    if (!observer.valid()) {
        return exitCode(ExitCode::EventFailed);
    }
    auto context = operationContext(*fixture.clock, "p10-process-crash-open");
    auto database = PersistenceWindows::WindowsCentralDatabase::open(
        fixture.paths,
        fixture.diagnostics,
        fixture.clock,
        context);
    if (!database) {
        return exitCode(ExitCode::OpenFailed);
    }
    auto repository = PersistenceWindows::WindowsAgentSessionRepository::create(
        std::move(database).value(), fixture.clock, &observer);
    if (!repository) {
        return exitCode(ExitCode::OpenFailed);
    }
    const auto mutation = startMutation(
        parse<Domain::SessionId>(arguments[6]),
        parse<Domain::ClientId>(arguments[7]),
        fixture.clock->utcNow());
    const auto started = repository.value()->startRun(
        mutation,
        operationContext(*fixture.clock, "p10-process-crash-start"));
    return started ? exitCode(ExitCode::Success) :
                     exitCode(ExitCode::OperationFailed);
}

[[nodiscard]] int crashDuringCompletion(
    wchar_t** const arguments,
    const PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint)
{
    RepositoryContext fixture{std::filesystem::path{arguments[2]}};
    BlockingTransactionObserver observer{
        arguments[4],
        arguments[5],
        PersistenceWindows::AgentSessionTransactionKind::Complete,
        checkpoint};
    if (!observer.valid()) {
        return exitCode(ExitCode::EventFailed);
    }
    auto context = operationContext(*fixture.clock, "p10-process-complete-open");
    auto database = PersistenceWindows::WindowsCentralDatabase::open(
        fixture.paths,
        fixture.diagnostics,
        fixture.clock,
        context);
    if (!database) {
        return exitCode(ExitCode::OpenFailed);
    }
    auto repository = PersistenceWindows::WindowsAgentSessionRepository::create(
        std::move(database).value(), fixture.clock, &observer);
    if (!repository) {
        return exitCode(ExitCode::OpenFailed);
    }
    const auto sessionId = parse<Domain::SessionId>(arguments[6]);
    const auto clientId = parse<Domain::ClientId>(arguments[7]);
    const auto startedAt = fixture.clock->utcNow();
    if (!repository.value()->startRun(
            startMutation(sessionId, clientId, startedAt),
            operationContext(*fixture.clock, "p10-process-complete-start"))) {
        return exitCode(ExitCode::OperationFailed);
    }
    const Domain::AgentRunCompleteMutation completion{
        sessionId,
        clientId,
        "{\"result\":\"crash-committed\"}",
        "crash-completion-summary",
        {},
        fixture.clock->utcNow()};
    const auto completed = repository.value()->completeRun(
        completion,
        operationContext(*fixture.clock, "p10-process-complete"));
    return completed ? exitCode(ExitCode::Success) :
                       exitCode(ExitCode::OperationFailed);
}

[[nodiscard]] Domain::AgentRunReattachMutation reattachMutation(
    PersistenceWindows::WindowsAgentSessionRepository& repository,
    const Domain::SessionId& sessionId,
    const Domain::ClientId& expectedClient,
    const Domain::ClientId& newClient,
    Contracts::IClock& clock)
{
    const auto existing = repository.getRun(
        sessionId,
        operationContext(clock, "p10-process-reattach-read"));
    if (!existing || !existing.value() || !existing.value()->goal) {
        throw std::runtime_error{"The process fixture could not read the run to reattach."};
    }
    const auto& run = *existing.value();
    Domain::ActiveBinding binding{
        run.session.id,
        run.session.agentId,
        *run.goal,
        {},
        {},
        run.outputSchema,
        {},
        run.workingDirectory};
    return Domain::AgentRunReattachMutation{
        sessionId,
        expectedClient,
        newClient,
        std::move(binding),
        Domain::makeAgentSupersedeSummary(
            "Closed because a process-fixture run was reattached",
            std::nullopt,
            std::optional<Domain::SessionId>{sessionId}),
        clock.utcNow()};
}

[[nodiscard]] int crashDuringReattach(
    wchar_t** const arguments,
    const PersistenceWindows::AgentSessionTransactionCheckpoint checkpoint)
{
    RepositoryContext fixture{std::filesystem::path{arguments[2]}};
    BlockingTransactionObserver observer{
        arguments[4],
        arguments[5],
        PersistenceWindows::AgentSessionTransactionKind::Reattach,
        checkpoint};
    if (!observer.valid()) {
        return exitCode(ExitCode::EventFailed);
    }
    auto database = PersistenceWindows::WindowsCentralDatabase::open(
        fixture.paths,
        fixture.diagnostics,
        fixture.clock,
        operationContext(*fixture.clock, "p10-process-reattach-crash-open"));
    if (!database) {
        return exitCode(ExitCode::OpenFailed);
    }
    auto repository = PersistenceWindows::WindowsAgentSessionRepository::create(
        std::move(database).value(), fixture.clock, &observer);
    if (!repository) {
        return exitCode(ExitCode::OpenFailed);
    }
    const auto mutation = reattachMutation(
        *repository.value(),
        parse<Domain::SessionId>(arguments[6]),
        parse<Domain::ClientId>(arguments[7]),
        parse<Domain::ClientId>(arguments[8]),
        *fixture.clock);
    const auto attached = repository.value()->reattachRun(
        mutation,
        operationContext(*fixture.clock, "p10-process-reattach-crash"));
    return attached ? exitCode(ExitCode::Success) :
                      exitCode(ExitCode::OperationFailed);
}

[[nodiscard]] int reattachConcurrently(wchar_t** const arguments)
{
    RepositoryContext fixture{std::filesystem::path{arguments[2]}};
    auto repository = PersistenceWindows::WindowsAgentSessionRepository::open(
        fixture.paths,
        fixture.diagnostics,
        fixture.clock,
        operationContext(*fixture.clock, "p10-process-reattach-open"));
    if (!repository) {
        return exitCode(ExitCode::OpenFailed);
    }
    InfrastructureDetail::UniqueHandle ready{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[3])};
    InfrastructureDetail::UniqueHandle start{
        ::OpenEventW(SYNCHRONIZE, FALSE, arguments[4])};
    if (!ready || !start || ::SetEvent(ready.get()) == FALSE) {
        return exitCode(ExitCode::EventFailed);
    }
    if (::WaitForSingleObject(start.get(), 30'000U) != WAIT_OBJECT_0) {
        return exitCode(ExitCode::EventFailed);
    }
    const auto sessionId = parse<Domain::SessionId>(arguments[5]);
    const auto expectedClient = parse<Domain::ClientId>(arguments[6]);
    const auto newClient = parse<Domain::ClientId>(arguments[7]);
    const auto mutation = reattachMutation(
        *repository.value(),
        sessionId,
        expectedClient,
        newClient,
        *fixture.clock);
    const auto attached = repository.value()->reattachRun(
        mutation,
        operationContext(*fixture.clock, "p10-process-reattach"));
    if (attached) {
        return exitCode(ExitCode::Success);
    }
    return attached.error().code == Domain::ErrorCodes::OwnershipConflict
        ? exitCode(ExitCode::OwnershipConflict)
        : exitCode(ExitCode::OperationFailed);
}

} // namespace

int wmain(const int argumentCount, wchar_t** const arguments)
{
    try {
        if (arguments == nullptr || argumentCount < 2 || arguments[1] == nullptr) {
            return exitCode(ExitCode::Usage);
        }
        const std::wstring_view mode{arguments[1]};
        if (mode == L"--crash-start-before-commit" && argumentCount == 8) {
            return crashDuringStart(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::BeforeCommit);
        }
        if (mode == L"--crash-start-after-commit" && argumentCount == 8) {
            return crashDuringStart(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::AfterCommit);
        }
        if (mode == L"--crash-complete-before-commit" && argumentCount == 8) {
            return crashDuringCompletion(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::BeforeCommit);
        }
        if (mode == L"--crash-complete-after-commit" && argumentCount == 8) {
            return crashDuringCompletion(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::AfterCommit);
        }
        if (mode == L"--crash-reattach-before-commit" && argumentCount == 9) {
            return crashDuringReattach(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::BeforeCommit);
        }
        if (mode == L"--crash-reattach-after-commit" && argumentCount == 9) {
            return crashDuringReattach(
                arguments,
                PersistenceWindows::AgentSessionTransactionCheckpoint::AfterCommit);
        }
        if (mode == L"--reattach" && argumentCount == 8) {
            return reattachConcurrently(arguments);
        }
        return exitCode(ExitCode::Usage);
    } catch (...) {
        return exitCode(ExitCode::UnexpectedFailure);
    }
}
