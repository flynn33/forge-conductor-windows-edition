#include "TestSupport.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FoundationFakes.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::LMStudioFallbackServerId;
using Infrastructure::Windows::LMStudioPrimaryServerId;
using Infrastructure::Windows::WindowsLMStudioHostActivator;
using Infrastructure::Windows::WindowsLMStudioHostActivatorOptions;
using Json = nlohmann::json;
using namespace std::chrono_literals;

static_assert(std::is_final_v<WindowsLMStudioHostActivator>);
static_assert(!std::is_copy_constructible_v<WindowsLMStudioHostActivator>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint now,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("85000000-0000-4000-8000-000000000001"),
        now + 5min,
        cancellation,
        parse<Domain::CorrelationId>("p15-lmstudio-host-activation")};
}

class MapFileSystem final : public Contracts::IFileSystem {
public:
    void seed(const std::string_view file, const std::string_view content)
    {
        std::vector<std::byte> encoded(content.size());
        if (!content.empty()) {
            std::memcpy(encoded.data(), content.data(), content.size());
        }
        files_.insert_or_assign(std::string{file}, std::move(encoded));
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext&) noexcept override
    {
        ++reads_;
        const auto found = files_.find(file.canonicalPath().value());
        if (found == files_.end()) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound, "Scripted synchronized state is absent."));
        }
        if (found->second.size() > maximumBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Scripted synchronized state is oversized."));
        }
        return Domain::Result<std::vector<std::byte>>::success(found->second);
    }

    [[nodiscard]] Domain::Result<void> writeFile(
        const Contracts::AuthorizedPath&,
        std::span<const std::byte>,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<Domain::DirectoryListing> list(
        const Contracts::AuthorizedPath&,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DirectoryListing>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "List is unsupported."));
    }

    [[nodiscard]] Domain::Result<void> createDirectory(
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Contracts::AuthorizedPath&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<void> move(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
    [[nodiscard]] static Domain::Result<void> unsupported()
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Mutation is unsupported."));
    }

    std::map<std::string, std::vector<std::byte>> files_;
    std::size_t reads_{};
};

class HostPlatformFake final : public Contracts::ILMStudioHostPlatform {
public:
    explicit HostPlatformFake(Fakes::FakeClock& clock) noexcept : clock_{clock} {}

    bool running{};
    std::function<void(std::size_t)> onWait;
    std::function<void()> beforeLaunch;

    void blockObservation() noexcept
    {
        std::scoped_lock lock{mutex_};
        blocking_ = true;
    }

    [[nodiscard]] bool waitUntilBlocked()
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, 5s, [&] { return activeWait_.has_value(); });
    }

    [[nodiscard]] Domain::Result<bool> isRunning(
        const Domain::PathText& executable,
        const Domain::OperationContext&) noexcept override
    {
        std::scoped_lock lock{mutex_};
        ++runningChecks_;
        lastExecutable_ = executable.value();
        return Domain::Result<bool>::success(running);
    }

    [[nodiscard]] Domain::Result<void> launch(
        const Domain::PathText& executable,
        const Domain::OperationContext& context) noexcept override
    {
        if (beforeLaunch) {
            beforeLaunch();
        }
        std::scoped_lock lock{mutex_};
        if (cancelledOperation_ && cancelledOperation_.value() == context.operationId) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "Scripted launch was cancelled at its side-effect boundary."));
        }
        ++launches_;
        lastExecutable_ = executable.value();
        running = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> waitForObservation(
        const std::chrono::milliseconds maximumWait,
        const Domain::OperationContext& context) noexcept override
    {
        std::size_t wait{};
        {
            std::scoped_lock lock{mutex_};
            wait = ++waits_;
        }
        if (onWait) {
            onWait(wait);
        }
        {
            std::unique_lock lock{mutex_};
            if (blocking_) {
                activeWait_ = context.operationId;
                changed_.notify_all();
                changed_.wait(lock, [&] {
                    return shutdown_ ||
                        (cancelledOperation_ &&
                         cancelledOperation_.value() == context.operationId);
                });
                activeWait_.reset();
                changed_.notify_all();
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "Scripted blocking observation was cancelled."));
            }
        }
        clock_.advance(maximumWait);
        std::scoped_lock lock{mutex_};
        if (cancelledOperation_ && cancelledOperation_.value() == context.operationId) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "Scripted observation wait was cancelled."));
        }
        return Domain::Result<void>::success();
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        {
            std::scoped_lock lock{mutex_};
            cancelledOperation_ = operationId;
        }
        changed_.notify_all();
    }

    void shutdown() noexcept override
    {
        {
            std::scoped_lock lock{mutex_};
            shutdown_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t launches() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return launches_;
    }
    [[nodiscard]] std::size_t runningChecks() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return runningChecks_;
    }
    [[nodiscard]] bool shutdownCalled() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return shutdown_;
    }
    [[nodiscard]] std::string lastExecutable() const
    {
        std::scoped_lock lock{mutex_};
        return lastExecutable_;
    }

private:
    Fakes::FakeClock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string lastExecutable_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::optional<Domain::OperationId> activeWait_;
    std::size_t runningChecks_{};
    std::size_t launches_{};
    std::size_t waits_{};
    bool blocking_{};
    bool shutdown_{};
};

struct Fixture final {
    Fixture()
        : now{std::chrono::steady_clock::now()},
          clock{Domain::UtcTimePoint{}, now},
          authorityProvider{
              parse<Domain::AuthorityId>("86000000-0000-4000-8000-000000000001"),
              parse<Domain::ClientId>("p15-lmstudio-maintenance"),
              {root},
              Domain::FileAccess::Execute,
              {Domain::FileAccess::Read, Domain::FileAccess::Execute},
              {},
              true,
              15U},
          authority{take(authorityProvider.authorityFor(projectId, context()))},
          platform{std::make_unique<HostPlatformFake>(clock)},
          platformView{platform.get()},
          activator{
              authorityProvider,
              files,
              clock,
              std::move(platform),
              WindowsLMStudioHostActivatorOptions{1ms, 64U * 1024U}}
    {
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::stop_token cancellation = {}) const
    {
        return ForgeConductor::Tests::context(now, cancellation);
    }

    Domain::MonotonicTimePoint now;
    Fakes::FakeClock clock;
    Domain::PathText root{path("C:\\lmstudio-test")};
    Domain::ProjectId projectId{
        parse<Domain::ProjectId>("87000000-0000-4000-8000-000000000001")};
    Fakes::DeterministicWorkspaceAuthority authorityProvider;
    Contracts::WorkspaceAuthority authority;
    MapFileSystem files;
    std::unique_ptr<HostPlatformFake> platform;
    HostPlatformFake* platformView{};
    WindowsLMStudioHostActivator activator;
};

[[nodiscard]] Domain::LMStudioEnvironmentStatus environment()
{
    return Domain::LMStudioEnvironmentStatus{
        true,
        path("C:\\lmstudio-test"),
        path("C:\\lmstudio-test\\mcp.json"),
        std::string{"0.4.21+2"},
        path("C:\\lmstudio-test\\LM Studio.exe"),
        {}};
}

[[nodiscard]] Domain::LMStudioHostActivationRequest request(
    const std::string_view deploymentId,
    const std::chrono::milliseconds timeout = 100ms)
{
    return Domain::LMStudioHostActivationRequest{
        parse<Domain::DeploymentId>(deploymentId), timeout};
}

[[nodiscard]] std::string synchronizedState(const std::string_view deploymentId)
{
    Json servers = Json::object();
    for (const auto [id, role] : {
             std::pair<std::string_view, std::string_view>{LMStudioPrimaryServerId, "primary"},
             std::pair<std::string_view, std::string_view>{LMStudioFallbackServerId, "fallback"}}) {
        servers[id] = Json{
            {"command", "C:\\Forge\\forge-conductor.exe"},
            {"args", Json::array({"serve"})},
            {"env", Json{
                {"FORGE_MCP_ROLE", role},
                {"FORGE_CONDUCTOR_HOME", "C:\\Forge\\home"},
                {"FORGE_DEPLOYMENT_ID", deploymentId}}}};
    }
    return Json{{"mcpServers", std::move(servers)}}.dump();
}

constexpr std::string_view SynchronizedPath{
    "C:\\lmstudio-test\\.internal\\last-synced-mcp-state.json"};
constexpr std::string_view LiveConfigurationPath{
    "C:\\lmstudio-test\\mcp.json"};

void testStoppedHostLaunchesAndAcceptsLazySynchronization()
{
    Fixture fixture;
    fixture.platformView->running = false;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("host-revision-1"));
    fixture.platformView->onWait = [&](const std::size_t wait) {
        if (wait == 1U) {
            fixture.files.seed(SynchronizedPath, synchronizedState("host-revision-1"));
        }
    };

    const auto result = take(fixture.activator.activate(
        environment(), request("host-revision-1"), fixture.authority,
        fixture.context()));
    require(!result.runningBeforeDeploy && result.launched && !result.restarted,
            "A stopped host was not launched through the supported one-way path.");
    require(result.configurationSynchronized && result.readyRoles.empty(),
            "Configuration synchronization was confused with lazy hosted role readiness.");
    require(fixture.platformView->launches() == 1U &&
                fixture.platformView->lastExecutable() ==
                    "C:\\lmstudio-test\\LM Studio.exe",
            "The activator did not launch the exact discovered executable once.");
}

void testRunningHostHotSynchronizesWithoutRestart()
{
    Fixture fixture;
    fixture.platformView->running = true;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("host-revision-hot"));
    fixture.files.seed(SynchronizedPath, synchronizedState("host-revision-hot"));
    const auto result = take(fixture.activator.activate(
        environment(), request("host-revision-hot"), fixture.authority,
        fixture.context()));
    require(result.runningBeforeDeploy && !result.launched && !result.restarted &&
                result.configurationSynchronized,
            "A running host did not use its supported hot-synchronization path.");
    require(fixture.platformView->launches() == 0U,
            "A running host was unnecessarily launched or restarted.");
}

void testWrongRevisionFailsWithoutUnsupportedRestart()
{
    Fixture fixture;
    fixture.platformView->running = true;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("wanted-revision"));
    fixture.files.seed(SynchronizedPath, synchronizedState("stale-revision"));
    const auto result = fixture.activator.activate(
        environment(), request("wanted-revision", 5ms), fixture.authority,
        fixture.context());
    requireError(result, Domain::ErrorCodes::AcknowledgementTimeout,
                 "A stale host-synchronized revision was accepted.");
    require(fixture.platformView->launches() == 0U,
            "The activator attempted an unsupported restart of a running host.");
}

void testDuplicateSyncKeysAreNeverAcknowledged()
{
    Fixture fixture;
    fixture.platformView->running = true;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("duplicate-revision"));
    const auto valid = synchronizedState("duplicate-revision");
    fixture.files.seed(
        SynchronizedPath,
        std::string{"{\"mcpServers\":{},\"mcpServers\":"} +
            Json::parse(valid).at("mcpServers").dump() + "}");
    requireError(
        fixture.activator.activate(
            environment(), request("duplicate-revision", 5ms),
            fixture.authority, fixture.context()),
        Domain::ErrorCodes::AcknowledgementTimeout,
        "Host synchronization accepted an ambiguous duplicate mcpServers key.");
    require(fixture.platformView->launches() == 0U,
            "Duplicate host evidence triggered an unsupported restart.");
}

void testSynchronizedStateMustMatchSelectedLiveCommandAndHome()
{
    Fixture fixture;
    fixture.platformView->running = true;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("matched-revision"));
    auto mismatched = Json::parse(synchronizedState("matched-revision"));
    mismatched["mcpServers"][LMStudioPrimaryServerId]["command"] =
        "C:\\Foreign\\forge-conductor.exe";
    mismatched["mcpServers"][LMStudioFallbackServerId]["command"] =
        "C:\\Foreign\\forge-conductor.exe";
    fixture.files.seed(SynchronizedPath, mismatched.dump());

    requireError(
        fixture.activator.activate(
            environment(), request("matched-revision", 5ms),
            fixture.authority, fixture.context()),
        Domain::ErrorCodes::AcknowledgementTimeout,
        "Host synchronization accepted a requested revision with a different command path.");
}

void testCancellationLinearizesBeforeLaunchAndWakesWait()
{
    {
        Fixture fixture;
        fixture.platformView->running = false;
        fixture.files.seed(
            LiveConfigurationPath, synchronizedState("launch-cancel"));
        const auto operation = fixture.context();
        fixture.platformView->beforeLaunch = [&] {
            fixture.activator.cancel(operation.operationId);
        };
        requireError(
            fixture.activator.activate(
                environment(), request("launch-cancel"),
                fixture.authority, operation),
            Domain::ErrorCodes::Cancelled,
            "Cancellation at the launch side-effect boundary was ignored.");
        require(fixture.platformView->launches() == 0U,
                "LM Studio was launched after cancellation linearized.");
    }

    {
        Fixture fixture;
        fixture.platformView->running = false;
        fixture.files.seed(
            LiveConfigurationPath, synchronizedState("token-launch-cancel"));
        std::stop_source cancellation;
        const auto operation = fixture.context(cancellation.get_token());
        fixture.platformView->beforeLaunch = [&] {
            cancellation.request_stop();
        };
        requireError(
            fixture.activator.activate(
                environment(), request("token-launch-cancel"),
                fixture.authority, operation),
            Domain::ErrorCodes::Cancelled,
            "Caller stop-token cancellation at the launch boundary was ignored.");
        require(fixture.platformView->launches() == 0U,
                "LM Studio was launched after caller cancellation linearized.");
    }

    {
        Fixture fixture;
        fixture.platformView->running = true;
        fixture.files.seed(
            LiveConfigurationPath, synchronizedState("wait-cancel"));
        const auto operation = fixture.context();
        fixture.platformView->onWait = [&](const std::size_t wait) {
            if (wait == 1U) {
                fixture.activator.cancel(operation.operationId);
            }
        };
        requireError(
            fixture.activator.activate(
                environment(), request("wait-cancel"),
                fixture.authority, operation),
            Domain::ErrorCodes::Cancelled,
            "Cancellation did not wake the host observation wait.");
    }
}

void testCancellationMissingEvidenceAndShutdown()
{
    Fixture fixture;
    auto missing = environment();
    missing.applicationExecutable.reset();
    requireError(
        fixture.activator.activate(
            missing, request("missing-evidence"), fixture.authority,
            fixture.context()),
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "Activation guessed an application path without discovery evidence.");

    std::stop_source cancelled;
    cancelled.request_stop();
    requireError(
        fixture.activator.activate(
            environment(), request("cancelled-revision"), fixture.authority,
            fixture.context(cancelled.get_token())),
        Domain::ErrorCodes::Cancelled,
        "Pre-cancelled host activation reached the platform boundary.");
    require(fixture.platformView->runningChecks() == 0U,
            "Pre-cancelled activation inspected host processes.");

    fixture.activator.shutdown();
    require(fixture.platformView->shutdownCalled(),
            "Activator shutdown did not wake and stop its owned platform seam.");
    requireError(
        fixture.activator.activate(
            environment(), request("after-shutdown"), fixture.authority,
            fixture.context()),
        Domain::ErrorCodes::Cancelled,
        "Activator accepted work after shutdown.");
}

void testShutdownCancelsAndDrainsActiveObservation()
{
    Fixture fixture;
    fixture.platformView->running = true;
    fixture.files.seed(
        LiveConfigurationPath, synchronizedState("shutdown-revision"));
    fixture.platformView->blockObservation();
    const auto operation = fixture.context();
    std::optional<Domain::Result<Domain::LMStudioHostActivationResult>> outcome;
    std::jthread activation{[&] {
        outcome.emplace(fixture.activator.activate(
            environment(), request("shutdown-revision"),
            fixture.authority, operation));
    }};
    require(fixture.platformView->waitUntilBlocked(),
            "Host activation did not enter its owned observation wait.");

    fixture.activator.shutdown();
    activation.join();
    require(outcome.has_value(),
            "Activator shutdown returned before the exact active operation drained.");
    requireError(
        outcome.value(),
        Domain::ErrorCodes::Cancelled,
        "Activator shutdown did not cancel the exact blocking observation.");
    require(fixture.platformView->shutdownCalled(),
            "Activator shutdown did not close its platform wait owner.");
}

} // namespace

void registerWindowsLMStudioHostActivatorTests(TestRegistry& tests)
{
    addTest(tests, "lmstudio.host.launch-and-lazy-sync",
            testStoppedHostLaunchesAndAcceptsLazySynchronization);
    addTest(tests, "lmstudio.host.hot-sync-no-restart",
            testRunningHostHotSynchronizesWithoutRestart);
    addTest(tests, "lmstudio.host.stale-revision-no-unsupported-restart",
            testWrongRevisionFailsWithoutUnsupportedRestart);
    addTest(tests, "lmstudio.host.duplicate-sync-keys",
            testDuplicateSyncKeysAreNeverAcknowledged);
    addTest(tests, "lmstudio.host.live-sync-identity",
            testSynchronizedStateMustMatchSelectedLiveCommandAndHome);
    addTest(tests, "lmstudio.host.atomic-launch-and-wakeable-cancel",
            testCancellationLinearizesBeforeLaunchAndWakesWait);
    addTest(tests, "lmstudio.host.cancel-evidence-shutdown",
            testCancellationMissingEvidenceAndShutdown);
    addTest(tests, "lmstudio.host.shutdown-drains-active-observation",
            testShutdownCancelsAndDrainsActiveObservation);
}

} // namespace ForgeConductor::Tests
