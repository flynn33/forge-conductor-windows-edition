#include "ForgeConductor/Application/DashboardConnectionApplication.h"
#include "ForgeConductor/Application/DashboardConnectionApplicationFactory.h"
#include "ForgeConductor/Dashboard/IDashboardConnectionApplicationFactory.h"
#include "ForgeConductor/Domain/Error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using namespace std::chrono_literals;

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::size_t line)
{
    throw std::runtime_error{
        "requirement failed at line " + std::to_string(line) + ": " +
        std::string{expression}};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::size_t line)
{
    ++assertions;
    if (!condition) fail(expression, line);
}

#define REQUIRE(condition) \
    require(static_cast<bool>(condition), #condition, __LINE__)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
[[nodiscard]] Domain::Result<Value> unexpectedDependencyCall() noexcept
{
    return Domain::Result<Value>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The factory test unexpectedly called an application dependency."));
}

class NoCallAssetStore final : public Dashboard::IDashboardAssetStore {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardStaticAssetHandle>
    findStaticAsset(
        const Dashboard::DashboardStaticResourcePath&) const noexcept override
    {
        return unexpectedDependencyCall<Dashboard::DashboardStaticAssetHandle>();
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardStaticAssetHandle>
    findShellAsset(Dashboard::DashboardShellAssetId) const noexcept override
    {
        return unexpectedDependencyCall<Dashboard::DashboardStaticAssetHandle>();
    }
};

class NoCallTelemetrySource final
    : public Dashboard::IDashboardTelemetrySource {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryHealth> health(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Dashboard::DashboardTelemetryHealth>();
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryObservation>
    latest(const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<
            Dashboard::DashboardTelemetryObservation>();
    }

    [[nodiscard]] Domain::Result<
        std::unique_ptr<Dashboard::IDashboardSseSubscription>>
    subscribe(
        const Dashboard::DashboardStreamRateSelection&,
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<
            std::unique_ptr<Dashboard::IDashboardSseSubscription>>();
    }

    void shutdown() noexcept override {}
};

class NoCallOperationalService final
    : public Dashboard::IDashboardOperationalService {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardStatusData> status(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Dashboard::DashboardStatusData>();
    }

    [[nodiscard]] Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Domain::DoctorReport>();
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<std::vector<Domain::AgentSpec>>();
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardSessionListing> sessions(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Dashboard::DashboardSessionListing>();
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<std::vector<Domain::AuditEvent>>();
    }

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<std::vector<std::string>>();
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<std::size_t>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Domain::AgentSession>();
    }

    void shutdown() noexcept override {}
};

class NoCallManagerClient final : public Contracts::IManagerClient {
public:
    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Domain::ManagerSettings>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return unexpectedDependencyCall<
            Domain::ManagerSettingsUpdateOutcome>();
    }

    [[nodiscard]] Domain::Result<void> requestRestart(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The factory test unexpectedly requested Manager restart."));
    }

    [[nodiscard]] Domain::Result<void> requestShutdown(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The factory test unexpectedly requested Manager shutdown."));
    }

    void shutdown() noexcept override {}
};

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::Sha256Digest bearerToken()
{
    return take(Domain::Sha256Digest::parse(std::string(64U, 'a')));
}

[[nodiscard]] Dashboard::DashboardApplicationIdentity identity()
{
    return Dashboard::DashboardApplicationIdentity{
        "Forge Conductor",
        "0.9.0",
        "windows-native",
        path("C:\\Users\\test\\ForgeConductor"),
        path("C:\\Users\\test\\ForgeConductor\\store"),
        42U};
}

[[nodiscard]] Domain::OperationContext context()
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        Domain::MonotonicTimePoint{} + 5min,
        {},
        take(Domain::CorrelationId::parse("dashboard-factory-test"))};
}

[[nodiscard]] Dashboard::DashboardHttpRequest request(std::string host)
{
    return Dashboard::DashboardHttpRequest{
        "GET", "/ping", {{"host", std::move(host)}}, {}};
}

[[nodiscard]] std::uint16_t status(
    Dashboard::IDashboardConnectionApplication& application,
    std::string host)
{
    auto exchange = take(application.prepare(
        request(std::move(host)), true, context()));
    REQUIRE(exchange.kind() == Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(exchange.completeExchange() != nullptr);
    const auto& bytes = exchange.completeExchange()->encodedResponse().bytes();
    REQUIRE(bytes.size() >= 12U);
    const std::string wire{
        reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    REQUIRE(wire.starts_with("HTTP/1.1 "));
    return static_cast<std::uint16_t>(std::stoul(wire.substr(9U, 3U)));
}

struct Fixture final {
    NoCallAssetStore assets;
    NoCallTelemetrySource telemetry;
    NoCallOperationalService operational;
    NoCallManagerClient manager;
    Application::DashboardConnectionApplicationFactory factory{
        Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB),
        identity(),
        bearerToken(),
        assets,
        telemetry,
        operational,
        manager};
};

void exposesClosedFactoryBoundary()
{
    using FactoryResult = Domain::Result<std::shared_ptr<
        Dashboard::IDashboardConnectionApplication>>;
    static_assert(std::is_abstract_v<
                  Dashboard::IDashboardConnectionApplicationFactory>);
    static_assert(std::is_final_v<
                  Application::DashboardConnectionApplicationFactory>);
    static_assert(std::is_base_of_v<
                  Dashboard::IDashboardConnectionApplicationFactory,
                  Application::DashboardConnectionApplicationFactory>);
    static_assert(!std::is_copy_constructible_v<
                  Application::DashboardConnectionApplicationFactory>);
    static_assert(!std::is_move_constructible_v<
                  Application::DashboardConnectionApplicationFactory>);
    static_assert(std::is_same_v<
                  decltype(std::declval<
                               Dashboard::IDashboardConnectionApplicationFactory&>()
                               .create(std::declval<
                                   const Domain::DashboardConfig&>())),
                  FactoryResult>);
    static_assert(noexcept(
        std::declval<Dashboard::IDashboardConnectionApplicationFactory&>()
            .create(std::declval<const Domain::DashboardConfig&>())));
}

void retainsOneExactEndpointPolicyPerGeneration()
{
    Fixture fixture;
    const Domain::DashboardConfig ipv4{
        "127.0.0.1", 47'820U, 8s};
    const Domain::DashboardConfig ipv6{"::1", 47'821U, 2s};

    auto first = take(fixture.factory.create(ipv4));
    auto second = take(fixture.factory.create(ipv6));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first != second);
    REQUIRE(dynamic_cast<Application::DashboardConnectionApplication*>(
                first.get()) != nullptr);
    REQUIRE(dynamic_cast<Application::DashboardConnectionApplication*>(
                second.get()) != nullptr);

    REQUIRE(status(*first, "127.0.0.1:47820") == 200U);
    REQUIRE(status(*first, "[::1]:47821") == 403U);
    REQUIRE(status(*second, "[::1]:47821") == 200U);
    REQUIRE(status(*second, "127.0.0.1:47820") == 403U);
}

void rejectsInvalidConfigurationBeforePublishingAnApplication()
{
    Fixture fixture;
    for (const auto& configuration : {
             Domain::DashboardConfig{"0.0.0.0", 47'820U, 8s},
             Domain::DashboardConfig{"127.0.0.1", 0U, 8s},
             Domain::DashboardConfig{"127.0.0.1", 47'820U, 0s},
             Domain::DashboardConfig{"::1", 47'820U, -1s}}) {
        auto result = fixture.factory.create(configuration);
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(!result.error().retryable);
    }
}

} // namespace

int main()
{
    try {
        exposesClosedFactoryBoundary();
        retainsOneExactEndpointPolicyPerGeneration();
        rejectsInvalidConfigurationBeforePublishingAnApplication();
        std::cout << "Dashboard connection application factory tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection application factory tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection application factory tests failed "
                     "with an unknown error.\n";
        return 1;
    }
}
