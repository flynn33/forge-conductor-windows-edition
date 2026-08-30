#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ManagerDashboardOperationalDataSource.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticLogTailReader.h"
#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"
#include "ForgeConductor/Persistence/Windows/WindowsClientPresenceRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsDashboardOperationalRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Composition = ForgeConductor::Composition::Windows;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace Persistence = ForgeConductor::Persistence::Windows;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

using namespace std::chrono_literals;

static_assert(
    std::is_final_v<Composition::ManagerDashboardOperationalDataSource>);
static_assert(std::is_base_of_v<
              Application::IDashboardOperationalDataSource,
              Composition::ManagerDashboardOperationalDataSource>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerDashboardOperationalDataSource>);
static_assert(!std::is_move_constructible_v<
              Composition::ManagerDashboardOperationalDataSource>);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::UtcTimePoint atSeconds(const std::int64_t seconds)
{
    return Domain::UtcTimePoint{std::chrono::seconds{seconds}};
}

[[nodiscard]] Domain::PathText pathText(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationContext cancelledContext()
{
    std::stop_source cancellation;
    cancellation.request_stop();
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "99999999-9999-4999-8999-999999999999"),
        std::chrono::steady_clock::now() + 1min,
        cancellation.get_token(),
        parse<Domain::CorrelationId>("manager-operational-cancelled")};
}

struct Fixture final {
    Fixture()
        : directory{L"P16-Manager-Operational-Adapter"},
          clock{std::make_shared<Support::FixedClock>(
              atSeconds(200), std::chrono::steady_clock::now())},
          paths{std::make_shared<Fakes::RecordingApplicationPathsFake>()},
          runtimeDiagnostics{std::make_shared<Fakes::RuntimeDiagnosticsFake>(
              clock->monotonicNow())},
          diagnosticsDirectory{directory.path() / L"diagnostics"}
    {
        require(
            std::filesystem::create_directories(diagnosticsDirectory),
            "diagnostics fixture directory was not created");
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(
            Domain::Result<Domain::PathText>::success(
                Support::pathText(directory.path())));

        auto opened = take(Persistence::WindowsCentralDatabase::open(
            paths,
            runtimeDiagnostics,
            clock,
            Support::activeContext("manager-operational-database-open")));
        database = std::shared_ptr<Persistence::WindowsCentralDatabase>{
            std::move(opened)};
        sessions = take(
            Persistence::WindowsAgentSessionRepository::attach(database, clock));
        presence = take(
            Persistence::WindowsClientPresenceRepository::attach(database));
        operational = take(
            Persistence::WindowsDashboardOperationalRepository::attach(
                database));
        diagnosticTail =
            std::make_unique<Infrastructure::WindowsDiagnosticLogTailReader>(
                Support::pathText(diagnosticsDirectory));
        adapter =
            std::make_unique<
                Composition::ManagerDashboardOperationalDataSource>(
                *operational, *sessions, *diagnosticTail, *clock);
    }

    ~Fixture() noexcept
    {
        adapter.reset();
        diagnosticTail.reset();
        if (operational) {
            operational->close();
        }
        if (presence) {
            presence->close();
        }
        if (sessions) {
            sessions->close();
        }
        if (database) {
            static_cast<void>(database->close(Support::activeContext(
                "manager-operational-database-close")));
        }
    }

    void saveSession(const Domain::AgentSession& session)
    {
        take(sessions->save(
            session,
            Support::activeContext("manager-operational-save-session")));
    }

    void savePresence(
        const std::string_view clientId,
        const std::string_view deploymentId,
        const std::uint32_t processId,
        const std::string_view workingDirectory,
        const Domain::UtcTimePoint firstSeenAt,
        const Domain::UtcTimePoint lastSeenAt)
    {
        take(presence->upsert(
            Domain::ClientPresenceRegistration{
                Domain::ClientPresenceIdentity{
                    parse<Domain::ClientId>(clientId),
                    "mcp",
                    parse<Domain::DeploymentId>(deploymentId),
                    processId},
                pathText(workingDirectory),
                firstSeenAt,
                lastSeenAt},
            Support::activeContext("manager-operational-save-presence")));
    }

    void writeDiagnostics(const std::string_view bytes)
    {
        std::ofstream output{
            diagnosticsDirectory / L"forge-diagnostics.jsonl",
            std::ios::binary | std::ios::trunc};
        require(output.good(), "diagnostic fixture did not open");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        require(output.good(), "diagnostic fixture did not flush");
    }

    Support::ScopedTestDirectory directory;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> runtimeDiagnostics;
    std::filesystem::path diagnosticsDirectory;
    std::shared_ptr<Persistence::WindowsCentralDatabase> database;
    std::shared_ptr<Persistence::WindowsAgentSessionRepository> sessions;
    std::shared_ptr<Persistence::WindowsClientPresenceRepository> presence;
    std::shared_ptr<Persistence::WindowsDashboardOperationalRepository>
        operational;
    std::unique_ptr<Infrastructure::WindowsDiagnosticLogTailReader>
        diagnosticTail;
    std::unique_ptr<Composition::ManagerDashboardOperationalDataSource>
        adapter;
};

[[nodiscard]] Domain::AgentSession session(
    const std::string_view sessionId,
    const Domain::SessionStatus status,
    std::optional<std::string> summary,
    const std::int64_t createdAt,
    const std::int64_t updatedAt)
{
    return Domain::AgentSession{
        parse<Domain::SessionId>(sessionId),
        parse<Domain::AgentId>("implement"),
        parse<Domain::ClientId>(
            "11111111-1111-4111-8111-111111111111"),
        status,
        std::move(summary),
        atSeconds(createdAt),
        atSeconds(updatedAt)};
}

void mapsTheAtomicProjectionWithoutReconstructingPresence()
{
    Fixture fixture;
    const auto expectedSession = session(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        Domain::SessionStatus::Open,
        std::nullopt,
        100,
        120);
    fixture.saveSession(expectedSession);
    fixture.savePresence(
        "11111111-1111-4111-8111-111111111111",
        "22222222-2222-4222-8222-222222222222",
        4242U,
        "C:\\Projects\\Forge",
        atSeconds(110),
        atSeconds(130));

    auto value = take(fixture.adapter->snapshot(
        Dashboard::DashboardApplicationLimits::MaximumOpenSessions,
        Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
        Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
        Support::activeContext("manager-operational-snapshot")));

    require(value.openSessions.size() == 1U, "open session was not mapped");
    require(value.recentSessions.size() == 1U, "recent session was not mapped");
    require(value.openSessions.front().id == expectedSession.id,
            "open session identity changed during mapping");
    require(value.recentSessions.front().id == expectedSession.id,
            "recent session identity changed during mapping");
    require(value.presence.size() == 1U, "presence row was not mapped");
    const auto& mappedPresence = value.presence.front();
    require(
        mappedPresence.clientId ==
            "11111111-1111-4111-8111-111111111111",
        "presence client identity changed");
    require(mappedPresence.hostKind == "mcp", "presence role changed");
    require(mappedPresence.processId == 4242U, "presence process changed");
    require(
        mappedPresence.workingDirectory.value() == "C:\\Projects\\Forge",
        "presence working directory changed");
    require(
        mappedPresence.lastHeartbeat == atSeconds(130),
        "presence heartbeat changed");

    fixture.saveSession(session(
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        Domain::SessionStatus::Active,
        std::nullopt,
        90,
        100));
    requireError(
        fixture.adapter->snapshot(
            1U,
            Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
            Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
            Support::activeContext("manager-operational-open-overflow")),
        Domain::ErrorCodes::LimitExceeded,
        "the adapter did not preserve the open-session overflow failure");
}

void routesBoundedDiagnosticTailReadsWithoutChangingRecords()
{
    Fixture fixture;
    fixture.writeDiagnostics("first\r\nsecond\nthird\r\n");

    const auto lines = take(fixture.adapter->diagnosticLines(
        2U,
        64U,
        128U,
        Support::activeContext("manager-operational-diagnostics")));
    require(
        lines == std::vector<std::string>{"second", "third"},
        "the adapter changed diagnostic tail ordering or content");

    requireError(
        fixture.adapter->diagnosticLines(
            Infrastructure::WindowsDiagnosticLogTailReader::
                    MaximumRequestedLines +
                1U,
            64U,
            128U,
            Support::activeContext("manager-operational-diagnostic-limit")),
        Domain::ErrorCodes::InvalidRequest,
        "the adapter did not preserve the diagnostic limit failure");
    requireError(
        fixture.adapter->diagnosticLines(2U, 64U, 128U, cancelledContext()),
        Domain::ErrorCodes::Cancelled,
        "the adapter did not preserve diagnostic cancellation");
}

void administrativelyClosesOnlyTheConcreteSessionAtTheInjectedClock()
{
    Fixture fixture;
    const auto prior = session(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        Domain::SessionStatus::Completed,
        std::string{"prior summary"},
        100,
        250);
    fixture.saveSession(prior);

    const Dashboard::DashboardSessionCloseRequest request{
        prior.id, "Operator supplied close summary"};
    const auto closed = take(fixture.adapter->closeSession(
        request,
        Support::activeContext("manager-operational-close")));
    require(closed.id == prior.id, "administrative close changed session id");
    require(
        closed.status == Domain::SessionStatus::Closed,
        "administrative close did not set closed status");
    require(
        closed.summary == request.summary(),
        "administrative close did not preserve the exact summary");
    require(
        closed.updatedAt == prior.updatedAt,
        "administrative close did not clamp the injected clock");

    const auto persisted = take(fixture.sessions->get(
        prior.id,
        Support::activeContext("manager-operational-read-closed")));
    require(persisted.has_value(), "closed session was not persisted");
    require(
        persisted->status == Domain::SessionStatus::Closed &&
            persisted->summary == request.summary() &&
            persisted->updatedAt == prior.updatedAt,
        "persisted administrative close differs from the adapter result");

    const Dashboard::DashboardSessionCloseRequest missing{
        parse<Domain::SessionId>(
            "cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        "missing"};
    requireError(
        fixture.adapter->closeSession(
            missing,
            Support::activeContext("manager-operational-close-missing")),
        Domain::ErrorCodes::SessionNotFound,
        "the adapter changed the concrete-session not-found result");
}

void preservesDependencyFailuresAndOwnsNoDependencyLifecycle()
{
    {
        Fixture fixture;
        fixture.operational->close();
        requireError(
            fixture.adapter->snapshot(
                1U,
                1U,
                1U,
                Support::activeContext("manager-operational-closed-source")),
            Domain::ErrorCodes::InvalidRequest,
            "the adapter changed an operational repository close failure");
    }
    {
        Fixture fixture;
        const auto expected = session(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            Domain::SessionStatus::Open,
            std::nullopt,
            100,
            120);
        fixture.saveSession(expected);
        fixture.adapter.reset();

        const auto source = take(fixture.operational->snapshot(
            1U,
            1U,
            1U,
            Support::activeContext("manager-operational-after-adapter")));
        require(
            source.openSessions.size() == 1U,
            "destroying the adapter closed its operational repository");
        const auto persisted = take(fixture.sessions->get(
            expected.id,
            Support::activeContext("manager-operational-session-after-adapter")));
        require(
            persisted.has_value(),
            "destroying the adapter closed its session repository");
        const auto lines = take(fixture.diagnosticTail->newestLines(
            1U,
            64U,
            64U,
            Support::activeContext("manager-operational-tail-after-adapter")));
        require(
            lines.empty(),
            "destroying the adapter changed its diagnostic reader");
    }
}

} // namespace

int main()
{
    try {
        mapsTheAtomicProjectionWithoutReconstructingPresence();
        routesBoundedDiagnosticTailReadsWithoutChangingRecords();
        administrativelyClosesOnlyTheConcreteSessionAtTheInjectedClock();
        preservesDependencyFailuresAndOwnsNoDependencyLifecycle();
        std::cout
            << "Manager dashboard operational data source tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Manager dashboard operational data source tests failed: "
            << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager dashboard operational data source tests failed "
                     "with an unknown error.\n";
        return 1;
    }
}
