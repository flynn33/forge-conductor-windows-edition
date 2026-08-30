#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::IWindowsLMStudioDiscoverySource;
using Infrastructure::Windows::WindowsLMStudioCandidateSelector;
using Infrastructure::Windows::WindowsLMStudioCandidateSelectorOptions;
using Infrastructure::Windows::WindowsLMStudioDiscoveryCandidate;
using Infrastructure::Windows::WindowsLMStudioDiscoverySource;
using Infrastructure::Windows::WindowsLMStudioEnvironment;
using Infrastructure::Windows::WindowsLMStudioEnvironmentOptions;

static_assert(std::is_final_v<WindowsLMStudioEnvironment>);
static_assert(!std::is_copy_constructible_v<WindowsLMStudioEnvironment>);
static_assert(std::is_final_v<WindowsLMStudioCandidateSelector>);
static_assert(!std::is_copy_constructible_v<WindowsLMStudioCandidateSelector>);
static_assert(
    WindowsLMStudioEnvironmentOptions::DefaultMaximumConfigurationBytes ==
    NativeTools::Windows::WindowsFileSystem::MaximumTextFileBytes);
static_assert(
    WindowsLMStudioCandidateSelectorOptions::
        DefaultMaximumConfigurationBytes ==
    WindowsLMStudioEnvironmentOptions::DefaultMaximumConfigurationBytes);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationContext activeContext(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("75000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + std::chrono::minutes{5},
        cancellation,
        parse<Domain::CorrelationId>("p15-lm-studio-discovery")};
}

class DiscoverySourceFake final : public IWindowsLMStudioDiscoverySource {
public:
    std::vector<WindowsLMStudioDiscoveryCandidate> scripted;

    [[nodiscard]] Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>> discover(
        const Domain::OperationContext& context) noexcept override
    {
        ++calls_;
        if (stopped_ || context.isCancellationRequested()) {
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled, "Discovery is stopped."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded, "Discovery expired."));
        }
        return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::success(scripted);
    }

    void shutdown() noexcept override { stopped_ = true; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }

private:
    std::size_t calls_{};
    bool stopped_{};
};

class MapFileSystem final : public Contracts::IFileSystem {
public:
    void seed(const Domain::PathText& file, const std::string_view content)
    {
        std::vector<std::byte> bytes(content.size());
        if (!content.empty()) {
            std::memcpy(bytes.data(), content.data(), content.size());
        }
        files_.insert_or_assign(file.value(), std::move(bytes));
    }

    void fail(const Domain::PathText& file, Domain::Error error)
    {
        failures_.insert_or_assign(file.value(), std::move(error));
    }

    void clearFailure(const Domain::PathText& file)
    {
        failures_.erase(file.value());
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        ++readCalls_;
        if (context.isCancellationRequested()) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled, "Read cancelled."));
        }
        if (const auto failure = failures_.find(file.canonicalPath().value());
            failure != failures_.end()) {
            return Domain::Result<std::vector<std::byte>>::failure(failure->second);
        }
        const auto match = files_.find(file.canonicalPath().value());
        if (match == files_.end()) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::RecordNotFound, "File not found."));
        }
        if (match->second.size() > maximumBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, "File too large."));
        }
        return Domain::Result<std::vector<std::byte>>::success(match->second);
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

    [[nodiscard]] std::size_t readCalls() const noexcept { return readCalls_; }

private:
    [[nodiscard]] static Domain::Result<void> unsupported()
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Mutation is unsupported."));
    }

    std::map<std::string, std::vector<std::byte>> files_;
    std::map<std::string, Domain::Error> failures_;
    std::size_t readCalls_{};
};

struct AuthorityFixture final {
    AuthorityFixture()
        : provider{
              parse<Domain::AuthorityId>("76000000-0000-4000-8000-000000000001"),
              parse<Domain::ClientId>("p15-lm-studio-maintenance"),
              {root},
              Domain::FileAccess::Write,
              {Domain::FileAccess::Read, Domain::FileAccess::Write},
              {},
              false,
              1U},
          authority{take(provider.authorityFor(project, activeContext()))}
    {
    }

    Domain::PathText root{path("C:\\lm-studio-test")};
    Domain::ProjectId project{
        parse<Domain::ProjectId>("77000000-0000-4000-8000-000000000001")};
    Fakes::DeterministicWorkspaceAuthority provider;
    Contracts::WorkspaceAuthority authority;
};

class SelectiveFaultWorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    SelectiveFaultWorkspaceAuthority(
        Contracts::IWorkspaceAuthority& delegate,
        Domain::PathText faultPath,
        Domain::Error fault)
        : delegate_{delegate}, faultPath_{std::move(faultPath)}, fault_{std::move(fault)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.authorityFor(projectId, context);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        const bool shellEnabled,
        const std::uint64_t generation,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.narrow(
            authority, trustedRoots, grants, shellEnabled, generation, context);
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        if (request.requestedPath == faultPath_) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(fault_);
        }
        return delegate_.authorize(authority, request, context);
    }

private:
    Contracts::IWorkspaceAuthority& delegate_;
    Domain::PathText faultPath_;
    Domain::Error fault_;
};

[[nodiscard]] WindowsLMStudioDiscoveryCandidate application(
    const Domain::LMStudioDiscoverySource source,
    const std::string_view executable,
    const bool valid,
    std::optional<std::string> version = std::nullopt,
    std::optional<Domain::PathText> root = std::nullopt,
    std::string detail = "Application candidate.")
{
    const auto executablePath = path(executable);
    return WindowsLMStudioDiscoveryCandidate{
        source,
        executablePath,
        executablePath,
        std::nullopt,
        std::move(root),
        std::move(version),
        valid,
        std::move(detail)};
}

[[nodiscard]] WindowsLMStudioDiscoveryCandidate configuration(
    const Domain::LMStudioDiscoverySource source,
    const std::string_view configurationPath,
    const bool valid,
    std::string detail = "Configuration candidate.")
{
    const auto candidate = path(configurationPath);
    return WindowsLMStudioDiscoveryCandidate{
        source,
        candidate,
        std::nullopt,
        candidate,
        std::nullopt,
        std::nullopt,
        valid,
        std::move(detail)};
}

[[nodiscard]] WindowsLMStudioDiscoveryCandidate unavailable(
    const Domain::LMStudioDiscoverySource source,
    const std::string_view evidencePath,
    std::string detail)
{
    return WindowsLMStudioDiscoveryCandidate{
        source,
        path(evidencePath),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        false,
        std::move(detail)};
}

[[nodiscard]] const Domain::LMStudioDiscoveryEvidence& evidenceFor(
    const Domain::LMStudioEnvironmentStatus& status,
    const std::string_view candidate)
{
    const auto found = std::find_if(
        status.discoveryEvidence.begin(),
        status.discoveryEvidence.end(),
        [&](const auto& evidence) { return evidence.path.value() == candidate; });
    require(found != status.discoveryEvidence.end(), "Expected discovery evidence is missing.");
    return *found;
}

void testCandidateSelectorRetainsExactBoundedSelectionEvidence()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    const auto configurationPath =
        path("C:\\lm-studio-test\\known\\mcp.json");
    const auto installedExecutable =
        path("C:\\lm-studio-test\\installed\\LM Studio.exe");
    const auto runningExecutable =
        path("C:\\lm-studio-test\\running\\LM Studio.exe");
    files.seed(configurationPath, R"({"mcpServers":{}})");

    WindowsLMStudioCandidateSelector selector{fixture.provider, files};
    auto selection = take(selector.select(
        {
            application(
                Domain::LMStudioDiscoverySource::RunningProcess,
                runningExecutable.value(),
                true,
                std::string{"0.4.20"}),
            configuration(
                Domain::LMStudioDiscoverySource::KnownUserLocation,
                configurationPath.value(),
                true),
            application(
                Domain::LMStudioDiscoverySource::InstalledApplication,
                installedExecutable.value(),
                true,
                std::string{"0.4.21"}),
        },
        fixture.authority,
        activeContext()));

    require(
        selection.authorityId() == fixture.authority.authorityId() &&
            selection.projectId() == fixture.authority.projectId() &&
            selection.callerId() == fixture.authority.callerId() &&
            selection.authorityGeneration() ==
                fixture.authority.generation(),
        "Candidate selection did not retain its exact authority binding.");
    require(
        selection.evaluations().size() == 3U &&
            selection.status().discoveryEvidence.size() == 3U,
        "Candidate selection did not retain one bounded evaluation per input.");
    require(
        selection.status().applicationExecutable == installedExecutable &&
            selection.status().configurationPath == configurationPath,
        "Candidate selection did not preserve environment precedence.");
    require(
        selection.evaluations()[0U].candidate().source ==
                Domain::LMStudioDiscoverySource::InstalledApplication &&
            selection.evaluations()[0U].selected() &&
            selection.evaluations()[1U].candidate().source ==
                Domain::LMStudioDiscoverySource::KnownUserLocation &&
            selection.evaluations()[1U].selected() &&
            !selection.evaluations()[2U].selected(),
        "Candidate evaluations did not retain stable priority and selection.");
    require(
        files.readCalls() == 1U,
        "Candidate selector did not perform the exact bounded configuration "
        "read.");
}

void testExplicitAndInstalledApplicationPrecedence()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    DiscoverySourceFake source;
    const auto explicitConfiguration = path("C:\\lm-studio-test\\explicit\\mcp.json");
    const auto knownConfiguration = path("C:\\lm-studio-test\\known\\mcp.json");
    const auto installedExecutable = path("C:\\lm-studio-test\\installed\\LM Studio.exe");
    const auto installedRoot = path("C:\\lm-studio-test\\installed");
    files.seed(explicitConfiguration, R"({"mcpServers":{},"foreign":{"kept":true}})");
    files.seed(knownConfiguration, R"({"mcpServers":{}})");
    source.scripted = {
        application(
            Domain::LMStudioDiscoverySource::RunningProcess,
            "C:\\lm-studio-test\\running\\LM Studio.exe",
            true,
            std::string{"0.4.20"}),
        configuration(
            Domain::LMStudioDiscoverySource::KnownUserLocation,
            knownConfiguration.value(),
            true),
        application(
            Domain::LMStudioDiscoverySource::InstalledApplication,
            installedExecutable.value(),
            true,
            std::string{"0.4.21+2"},
            installedRoot)};

    WindowsLMStudioEnvironment environment{fixture.provider, files, source};
    const auto status = take(environment.inspect(
        explicitConfiguration, fixture.authority, activeContext()));
    require(status.lmStudioPresent, "A valid installed LM Studio executable was not detected.");
    require(status.configurationPath && *status.configurationPath == explicitConfiguration,
            "The valid explicit configuration did not take precedence.");
    require(status.applicationExecutable && *status.applicationExecutable == installedExecutable,
            "The installed-application executable did not outrank the running image.");
    require(status.installationRoot && *status.installationRoot == installedRoot,
            "installationRoot is not the selected executable's parent directory.");
    require(status.version == std::optional<std::string>{"0.4.21+2"},
            "The selected installed-application version was not retained.");
    require(status.discoveryEvidence.size() == 4U,
            "Discovery did not retain every inspected candidate.");
    require(status.discoveryEvidence.front().source ==
                Domain::LMStudioDiscoverySource::ExplicitConfiguration,
            "Discovery evidence was not ordered by precedence.");
    require(evidenceFor(status, explicitConfiguration.value()).selected,
            "Explicit configuration evidence was not marked selected.");
    require(evidenceFor(status, installedExecutable.value()).selected,
            "Installed executable evidence was not marked selected.");
    require(!evidenceFor(status, knownConfiguration.value()).selected,
            "A lower-precedence configuration was incorrectly selected.");
    require(files.readCalls() == 2U,
            "Discovery did not strictly parse both valid configuration candidates.");
}

void testInvalidCandidatesFallBackWithEvidence()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    DiscoverySourceFake source;
    const auto explicitConfiguration = path("C:\\lm-studio-test\\explicit\\mcp.json");
    const auto knownConfiguration = path("C:\\lm-studio-test\\known\\mcp.json");
    const auto knownExecutable = path("C:\\lm-studio-test\\known\\LM Studio.exe");
    files.seed(
        explicitConfiguration,
        R"({"mcpServers":{},"mcpServers":{"duplicate":{}}})");
    files.seed(knownConfiguration, R"({"mcpServers":{"foreign":{"command":"safe"}}})");
    source.scripted = {
        unavailable(
            Domain::LMStudioDiscoverySource::RunningProcess,
            "LM Studio.exe",
            "Process image inspection was denied."),
        application(
            Domain::LMStudioDiscoverySource::InstalledApplication,
            "C:\\lm-studio-test\\missing\\LM Studio.exe",
            false),
        configuration(
            Domain::LMStudioDiscoverySource::KnownUserLocation,
            knownConfiguration.value(),
            true),
        application(
            Domain::LMStudioDiscoverySource::KnownUserLocation,
            knownExecutable.value(),
            true,
            std::string{"0.4.21+2"})};

    WindowsLMStudioEnvironment environment{fixture.provider, files, source};
    const auto status = take(environment.inspect(
        explicitConfiguration, fixture.authority, activeContext()));
    require(status.configurationPath && *status.configurationPath == knownConfiguration,
            "Malformed explicit JSON did not fall back to the valid known configuration.");
    require(status.applicationExecutable && *status.applicationExecutable == knownExecutable,
            "An invalid installed candidate prevented the known executable fallback.");
    require(status.installationRoot &&
                status.installationRoot->value() == "C:\\lm-studio-test\\known",
            "The executable parent was not derived when the source omitted installationRoot.");
    require(!evidenceFor(status, explicitConfiguration.value()).valid,
            "Duplicate JSON keys were accepted as a valid configuration.");
    require(!evidenceFor(status, "LM Studio.exe").valid,
            "Denied running-process discovery was reported as valid.");
    require(evidenceFor(status, knownConfiguration.value()).selected,
            "The fallback configuration evidence was not marked selected.");
}

void testCandidateAccessFallbackAndSystemicFaultPropagation()
{
    AuthorityFixture fixture;
    const auto explicitConfiguration = path("C:\\lm-studio-test\\explicit\\mcp.json");
    const auto knownConfiguration = path("C:\\lm-studio-test\\known\\mcp.json");
    DiscoverySourceFake source;
    source.scripted = {
        configuration(
            Domain::LMStudioDiscoverySource::KnownUserLocation,
            knownConfiguration.value(),
            true)};

    for (const auto code : {
             Domain::ErrorCodes::Unauthorized,
             Domain::ErrorCodes::PathOutsideAuthority}) {
        MapFileSystem files;
        files.seed(knownConfiguration, R"({"mcpServers":{}})");
        SelectiveFaultWorkspaceAuthority localFault{
            fixture.provider,
            explicitConfiguration,
            Domain::makeError(code, "Candidate-local authorization rejection.")};
        WindowsLMStudioEnvironment environment{localFault, files, source};
        const auto status = take(environment.inspect(
            explicitConfiguration, fixture.authority, activeContext()));
        require(status.configurationPath && *status.configurationPath == knownConfiguration,
                "A candidate-local authorization rejection prevented evidence fallback.");
        require(!evidenceFor(status, explicitConfiguration.value()).valid,
                "A rejected explicit candidate was reported as valid.");
    }

    {
        MapFileSystem files;
        files.seed(knownConfiguration, R"({"mcpServers":{}})");
        SelectiveFaultWorkspaceAuthority systemicAuthorityFault{
            fixture.provider,
            explicitConfiguration,
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Injected authority boundary failure.")};
        WindowsLMStudioEnvironment environment{
            systemicAuthorityFault, files, source};
        requireError(
            environment.inspect(
                explicitConfiguration, fixture.authority, activeContext()),
            Domain::ErrorCodes::InternalFailure,
            "An authority InternalFailure was softened into fallback evidence.");
    }

    for (const auto code : {
             Domain::ErrorCodes::InternalFailure,
             Domain::ErrorCodes::TransportClosed,
             Domain::ErrorCodes::StorageFull}) {
        MapFileSystem files;
        files.fail(
            explicitConfiguration,
            Domain::makeError(code, "Injected filesystem boundary failure."));
        files.seed(knownConfiguration, R"({"mcpServers":{}})");
        WindowsLMStudioEnvironment environment{fixture.provider, files, source};
        const auto result = environment.inspect(
            explicitConfiguration, fixture.authority, activeContext());
        requireError(
            result,
            code,
            "A systemic filesystem error was softened into fallback evidence.");
    }
}

void testMissingHostIsReportedWithoutGuessing()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    DiscoverySourceFake source;
    const auto missingConfiguration = path("C:\\lm-studio-test\\missing\\mcp.json");
    source.scripted = {
        unavailable(
            Domain::LMStudioDiscoverySource::InstalledApplication,
            "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            "No registration."),
        configuration(
            Domain::LMStudioDiscoverySource::KnownUserLocation,
            missingConfiguration.value(),
            true),
        unavailable(
            Domain::LMStudioDiscoverySource::RunningProcess,
            "LM Studio.exe",
            "No process.")};

    WindowsLMStudioEnvironment environment{fixture.provider, files, source};
    const auto status = take(environment.inspect(
        std::nullopt, fixture.authority, activeContext()));
    require(!status.lmStudioPresent && !status.applicationExecutable &&
                !status.installationRoot && !status.version,
            "Missing LM Studio was reported as installed.");
    require(!status.configurationPath,
            "An unreadable known configuration path was selected by resemblance alone.");
    require(status.discoveryEvidence.size() == 3U,
            "Missing-host evidence was not retained.");
    require(!evidenceFor(status, missingConfiguration.value()).valid,
            "A missing configuration file was reported as valid.");
}

void testConnectionHealthSnapshotIsBoundedAndCached()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    DiscoverySourceFake source;
    WindowsLMStudioEnvironment environment{fixture.provider, files, source};
    const auto context = activeContext();

    const auto initial = take(environment.connectionHealth(context));
    require(initial.roles.empty() &&
                initial.state == Domain::LMStudioConnectionState::Unavailable,
            "The initial connection-health cache was not unavailable.");

    Domain::LMStudioConnectionHealth ready{
        {
            Domain::LMStudioConnectorHealth{
                Domain::LMStudioConnectorRole::Primary,
                true,
                std::string{"2025-11-25"},
                53U,
                "Primary role ready."},
            Domain::LMStudioConnectorHealth{
                Domain::LMStudioConnectorRole::Fallback,
                true,
                std::string{"2025-11-25"},
                53U,
                "Fallback role ready."},
        },
        Domain::LMStudioConnectionState::Ready};
    require(environment.cacheConnectionHealth(ready, context).hasValue(),
            "A consistent measured role-health snapshot was rejected.");
    const auto cached = take(environment.connectionHealth(context));
    require(cached.state == Domain::LMStudioConnectionState::Ready &&
                cached.roles.size() == 2U && cached.roles[0].toolCount == 53U,
            "The connection-health snapshot was not cached exactly.");

    auto inconsistent = ready;
    inconsistent.state = Domain::LMStudioConnectionState::Unavailable;
    requireError(
        environment.cacheConnectionHealth(std::move(inconsistent), context),
        Domain::ErrorCodes::InvalidRequest,
        "An inconsistent connection-health snapshot was cached.");

    auto wrongProtocol = ready;
    wrongProtocol.roles[0].protocolVersion = "2025-06-18";
    requireError(
        environment.cacheConnectionHealth(std::move(wrongProtocol), context),
        Domain::ErrorCodes::InvalidRequest,
        "A ready role with an unsupported MCP protocol was cached.");

    auto wrongToolCount = ready;
    wrongToolCount.roles[1].toolCount = 52U;
    requireError(
        environment.cacheConnectionHealth(std::move(wrongToolCount), context),
        Domain::ErrorCodes::InvalidRequest,
        "A ready role with an incomplete tool catalog was cached.");

    const Domain::LMStudioConnectionHealth staleNonReady{
        {Domain::LMStudioConnectorHealth{
            Domain::LMStudioConnectorRole::Primary,
            false,
            std::string{"2025-11-25"},
            53U,
            "Primary role failed after a prior measurement."}},
        Domain::LMStudioConnectionState::Unavailable};
    requireError(
        environment.cacheConnectionHealth(staleNonReady, context),
        Domain::ErrorCodes::InvalidRequest,
        "A non-ready role retained stale protocol and tool readiness evidence.");

    environment.shutdown();
    requireError(
        environment.connectionHealth(context),
        Domain::ErrorCodes::TransportClosed,
        "The environment returned cached health after shutdown.");
    require(!source.stopped(),
            "The environment incorrectly shut down its borrowed discovery source.");
}

void testCancellationAndCandidateBound()
{
    AuthorityFixture fixture;
    MapFileSystem files;
    DiscoverySourceFake source;
    source.scripted = {
        unavailable(Domain::LMStudioDiscoverySource::InstalledApplication, "one", "one"),
        unavailable(Domain::LMStudioDiscoverySource::KnownUserLocation, "two", "two"),
        unavailable(Domain::LMStudioDiscoverySource::RunningProcess, "three", "three")};
    WindowsLMStudioEnvironment bounded{
        fixture.provider,
        files,
        source,
        WindowsLMStudioEnvironmentOptions{2U, 1024U, 8U}};
    requireError(
        bounded.inspect(std::nullopt, fixture.authority, activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "The candidate bound was not enforced.");

    DiscoverySourceFake cancelledSource;
    WindowsLMStudioEnvironment cancelled{fixture.provider, files, cancelledSource};
    std::stop_source stop;
    stop.request_stop();
    requireError(
        cancelled.inspect(std::nullopt, fixture.authority, activeContext(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "A cancelled environment inspection reached discovery.");
    require(cancelledSource.calls() == 0U,
            "Cancellation was not checked before invoking the discovery source.");
}

void testNativeSourceReadOnlySmoke()
{
    WindowsLMStudioDiscoverySource source;
    const auto candidates = take(source.discover(activeContext()));
    require(!candidates.empty(),
            "Native discovery did not retain evidence for any discovery source.");
    require(candidates.size() <=
                Infrastructure::Windows::WindowsLMStudioDiscoverySourceOptions::
                    DefaultMaximumCandidates,
            "Native discovery exceeded its default candidate bound.");
    for (const auto& candidate : candidates) {
        require(candidate.source != Domain::LMStudioDiscoverySource::ExplicitConfiguration,
                "The native source fabricated an explicit-configuration candidate.");
        const std::size_t resources =
            static_cast<std::size_t>(candidate.applicationExecutable.has_value()) +
            static_cast<std::size_t>(candidate.configurationPath.has_value());
        require(!candidate.valid || resources == 1U,
                "A valid native candidate did not identify exactly one resource.");
    }
    source.shutdown();
    requireError(
        source.discover(activeContext()),
        Domain::ErrorCodes::TransportClosed,
        "The native discovery source accepted work after shutdown.");
}

} // namespace

void registerWindowsLMStudioEnvironmentTests(TestRegistry& tests)
{
    addTest(
        tests,
        "lm_studio_environment.public_candidate_selector_evidence",
        testCandidateSelectorRetainsExactBoundedSelectionEvidence);
    addTest(
        tests,
        "lm_studio_environment.explicit_and_installed_precedence",
        testExplicitAndInstalledApplicationPrecedence);
    addTest(
        tests,
        "lm_studio_environment.invalid_candidates_fall_back_with_evidence",
        testInvalidCandidatesFallBackWithEvidence);
    addTest(
        tests,
        "lm_studio_environment.candidate_fallback_and_systemic_faults",
        testCandidateAccessFallbackAndSystemicFaultPropagation);
    addTest(
        tests,
        "lm_studio_environment.missing_host_without_guessing",
        testMissingHostIsReportedWithoutGuessing);
    addTest(
        tests,
        "lm_studio_environment.connection_health_cache",
        testConnectionHealthSnapshotIsBoundedAndCached);
    addTest(
        tests,
        "lm_studio_environment.cancellation_and_candidate_bound",
        testCancellationAndCandidateBound);
    addTest(
        tests,
        "lm_studio_environment.native_source_read_only_smoke",
        testNativeSourceReadOnlySmoke);
}

} // namespace ForgeConductor::Tests
