#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpInvocationGuard.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpToolPackAdapter.h"
#include "ForgeConductor/Mcp/McpToolRouter.h"
#include "Fakes/ApplicationServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/FileSystemFake.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/GitServiceFake.h"
#include "Fakes/PdfServiceFake.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/RecordingContinuityCoordinator.h"
#include "Fakes/RecordingProjectMemoryService.h"
#include "Fakes/ShellServiceFake.h"
#include "Fakes/TextSearchServiceFake.h"
#include "Fakes/ToolServiceFakes.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
namespace Fakes = ForgeConductor::Tests::Fakes;
using Json = nlohmann::json;

using namespace std::chrono_literals;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +     \
                                     #condition};                                \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] Domain::Result<T> unavailable(const char* message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, message));
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

class PassiveReportInspector final
    : public Contracts::IAgentCompletionReportInspector {
public:
    [[nodiscard]] Domain::Result<std::vector<Domain::AgentReportField>> inspect(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<std::vector<Domain::AgentReportField>>(
            "The report inspector is not configured for this test.");
    }
};

class PassiveLegacyContinuity final
    : public Contracts::ILegacyContextContinuityService {
public:
    void setGetOutcome(Domain::LegacyContinuityGetOutcome outcome)
    {
        getOutcome_ = std::move(outcome);
    }

    void setStatusSummary(Domain::LegacyContinuityStatusSummary summary)
    {
        statusSummary_ = std::move(summary);
    }

    void setAutomaticOutcome(Domain::LegacyContinuityPersistOutcome outcome)
    {
        automaticOutcome_ = std::move(outcome);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest& request,
        const Domain::ClientId& clientId,
        const Domain::OperationContext&) noexcept override
    {
        ++automaticCalls_;
        lastAutomaticRequest_ = request;
        lastAutomaticClientId_ = clientId;
        if (automaticOutcome_) {
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                    *automaticOutcome_);
        }
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId&,
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest&,
        const Domain::OperationContext&) noexcept override
    {
        if (getOutcome_) {
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success(
                *getOutcome_);
        }
        return unavailable<Domain::LegacyContinuityGetOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityListOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityStatusSummary>
    statusSummary(const Domain::OperationContext&) noexcept override
    {
        if (statusSummary_) {
            return Domain::Result<
                Domain::LegacyContinuityStatusSummary>::success(
                    *statusSummary_);
        }
        return unavailable<Domain::LegacyContinuityStatusSummary>(message_);
    }

    [[nodiscard]]
    Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repairProjections(const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityProjectionRepairOutcome>(
            message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityResetOutcome>(message_);
    }

    void shutdown() noexcept override {}

    [[nodiscard]] std::size_t automaticCalls() const noexcept
    {
        return automaticCalls_;
    }

    [[nodiscard]] const std::optional<Domain::LegacyContinuityAutomaticRequest>&
    lastAutomaticRequest() const noexcept
    {
        return lastAutomaticRequest_;
    }

    [[nodiscard]] const std::optional<Domain::ClientId>&
    lastAutomaticClientId() const noexcept
    {
        return lastAutomaticClientId_;
    }

private:
    static constexpr const char* message_ =
        "Legacy continuity is not configured for this test.";
    std::optional<Domain::LegacyContinuityGetOutcome> getOutcome_;
    std::optional<Domain::LegacyContinuityStatusSummary> statusSummary_;
    std::optional<Domain::LegacyContinuityPersistOutcome> automaticOutcome_;
    std::optional<Domain::LegacyContinuityAutomaticRequest>
        lastAutomaticRequest_;
    std::optional<Domain::ClientId> lastAutomaticClientId_;
    std::size_t automaticCalls_{};
};

class RecordingClientWorkspaceContext final
    : public Contracts::IMcpClientWorkspaceContext {
public:
    void setAdoption(Domain::ClientWorkspaceAdoption adoption)
    {
        adoption_ = std::move(adoption);
    }

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId& clientId,
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext&) noexcept override
    {
        ++adoptCalls_;
        lastClientId_ = clientId;
        lastHandoffId_ = record.packet.id;
        return Domain::Result<Domain::ClientWorkspaceAdoption>::success(
            adoption_);
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>> snapshot(
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<
            std::optional<Domain::ClientWorkspaceSnapshot>>::success(
                adoption_.snapshot);
    }

    void clear(const Domain::ClientId&) noexcept override {}
    void shutdown() noexcept override {}

    [[nodiscard]] std::size_t adoptCalls() const noexcept
    {
        return adoptCalls_;
    }

    [[nodiscard]] const std::optional<Domain::ClientId>&
    lastClientId() const noexcept
    {
        return lastClientId_;
    }

    [[nodiscard]] const std::optional<Domain::LegacyHandoffId>&
    lastHandoffId() const noexcept
    {
        return lastHandoffId_;
    }

private:
    Domain::ClientWorkspaceAdoption adoption_;
    std::size_t adoptCalls_{};
    std::optional<Domain::ClientId> lastClientId_;
    std::optional<Domain::LegacyHandoffId> lastHandoffId_;
};

class PassiveFileTextServices final
    : public Contracts::ITextFileEditService,
      public Contracts::IPathGlobService {
public:
    void setReplaceAllResult(
        Domain::Result<Contracts::TextFileEditReport> result)
    {
        replaceAllResult_ = std::move(result);
    }

    [[nodiscard]] Domain::Result<Contracts::TextFileEditReport> replaceAll(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        std::string_view,
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        if (replaceAllResult_) {
            return *replaceAllResult_;
        }
        return unavailable<Contracts::TextFileEditReport>(message_);
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::PathText>> glob(
        const Contracts::AuthorizedPath&,
        std::string_view,
        std::size_t,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<std::vector<Domain::PathText>>(message_);
    }

private:
    static constexpr const char* message_ =
        "Text editing and globbing are not configured for this test.";
    std::optional<Domain::Result<Contracts::TextFileEditReport>>
        replaceAllResult_;
};

class PassiveContinuityCodec final
    : public Contracts::IContinuityDocumentCodec {
public:
    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext&) noexcept override
    {
        try {
            return Domain::Result<Contracts::ContinuityDocument>::success(
                Contracts::ContinuityDocument{handoff, "{}"});
        } catch (...) {
            return unavailable<Contracts::ContinuityDocument>(message_);
        }
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> decode(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Contracts::ContinuityDocument>(message_);
    }

private:
    static constexpr const char* message_ =
        "The continuity codec is not configured for this test.";
};

class PassiveContinuityAutomation final
    : public Contracts::IContinuityAutomationStatusSource {
public:
    void setSnapshot(Domain::ContinuityAutomationStatusSnapshot snapshot)
    {
        snapshot_ = std::move(snapshot);
    }

    [[nodiscard]] Domain::ContinuityAutomationStatusSnapshot snapshot(
        const Domain::ClientId&) const noexcept override
    {
        return snapshot_;
    }
private:
    Domain::ContinuityAutomationStatusSnapshot snapshot_;
};

class RecordingForgeStatusRepository final
    : public Contracts::IForgeStatusRepository {
public:
    void setProjection(Domain::ForgeStatusProjection projection)
    {
        projection_ = std::move(projection);
        failureCode_.reset();
    }

    void setFailure(std::string code)
    {
        failureCode_ = std::move(code);
    }

    [[nodiscard]] Domain::Result<Domain::ForgeStatusProjection> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        ++calls_;
        if (failureCode_) {
            return Domain::Result<Domain::ForgeStatusProjection>::failure(
                Domain::makeError(
                    *failureCode_,
                    "The scripted Forge status projection failed."));
        }
        return Domain::Result<Domain::ForgeStatusProjection>::success(
            projection_);
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    Domain::ForgeStatusProjection projection_;
    std::optional<std::string> failureCode_;
    std::size_t calls_{};
    bool closed_{};
};

void testHandlerContract()
{
    static_assert(std::is_final_v<Mcp::McpToolPackAdapter>);
    static_assert(std::derived_from<
                  Mcp::McpToolPackAdapter,
                  Contracts::IToolHandler>);
    static_assert(!std::is_copy_constructible_v<Mcp::McpToolPackAdapter>);
    static_assert(!std::is_move_constructible_v<Mcp::McpToolPackAdapter>);
    static_assert(std::is_aggregate_v<Mcp::McpToolPackDependencies>);

    using HandleSignature = Domain::Result<Domain::ToolCallOutcome>
        (Mcp::McpToolPackAdapter::*)(
            const Contracts::AuthorizedToolCall&,
            const Contracts::WorkspaceAuthority&,
            const Domain::OperationContext&) noexcept;
    const HandleSignature handle = &Mcp::McpToolPackAdapter::handle;
    REQUIRE(handle != nullptr);
}

void testAllCatalogPacksAreBoundedByTheAdapterContract()
{
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();
    REQUIRE(tools.size() == 53U);

    const std::map<std::string_view, std::size_t> expectedPackCounts{
        {"AgentToolPack", 8U},
        {"ContinuityLifecycleToolPack", 7U},
        {"ContinuityToolPack", 4U},
        {"DocsToolPack", 2U},
        {"FilesystemToolPack", 8U},
        {"GitToolPack", 5U},
        {"MemoryToolPack", 5U},
        {"ProjectMemoryToolPack", 12U},
        {"SearchToolPack", 1U},
        {"ShellToolPack", 1U}};
    std::map<std::string_view, std::size_t> actualPackCounts;
    for (const auto& descriptor : tools) {
        ++actualPackCounts[descriptor.tool.pack];
        REQUIRE(descriptor.tool.availability ==
                Domain::ToolAvailability::Available);
        const auto schema = Json::parse(descriptor.inputSchema);
        REQUIRE(schema.is_object());
        REQUIRE(schema.value("type", "") == "object");

        const bool closedPack =
            descriptor.tool.pack == "ProjectMemoryToolPack" ||
            descriptor.tool.pack == "ContinuityLifecycleToolPack";
        REQUIRE(schema.value("additionalProperties", true) != closedPack);
    }
    REQUIRE(actualPackCounts == expectedPackCounts);
}

void testRuntimeDispatchAndSchemaPolicy()
{
    const auto authorityId = parse<Domain::AuthorityId>(
        "10000000-0000-4000-8000-000000000001");
    const auto projectId = parse<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000002");
    const auto operationId = parse<Domain::OperationId>(
        "30000000-0000-4000-8000-000000000003");
    const auto correlationId = parse<Domain::CorrelationId>("adapter-runtime");
    const auto clientId = parse<Domain::ClientId>("adapter-client");
    const auto root = take(Domain::PathText::create("D:/workspace"));
    const Domain::OperationContext context{
        operationId,
        Domain::MonotonicTimePoint{} + 5min,
        {},
        correlationId};

    Fakes::DeterministicWorkspaceAuthority workspaceAuthority{
        authorityId,
        clientId,
        {root},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        {},
        true,
        11U};
    auto authority = take(workspaceAuthority.authorityFor(projectId, context));
    Fakes::DeterministicWorkspaceAuthority shellAuthorityIssuer{
        authorityId,
        clientId,
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        {},
        true,
        11U};
    auto shellAuthority = take(
        shellAuthorityIssuer.authorityFor(projectId, context));

    auto catalog = take(Mcp::McpToolCatalog::create());
    Fakes::RecordingApplicationPathsFake applicationPaths;
    Fakes::RecordingAgentCatalogFake agentCatalog;
    applicationPaths.dataRootResult.set(
        Domain::Result<Domain::PathText>::success(root));
    agentCatalog.allResult.set(
        Domain::Result<std::vector<Domain::AgentSpec>>::success({}));
    Fakes::RecordingAgentSessionServiceFake agentSessions;
    PassiveReportInspector reportInspector;
    PassiveLegacyContinuity legacyContinuity;
    RecordingClientWorkspaceContext clientWorkspaceContext;
    Fakes::RecordingFileSystemFake fileSystem{3U * 1024U * 1024U};
    PassiveFileTextServices fileTextServices;
    Fakes::RecordingGitServiceFake git;
    Fakes::LegacyMemoryServiceFake legacyMemory{
        32U,
        Domain::DestructiveConfirmation{
            "purge_legacy_memory", "all", "test-token"}};
    Fakes::RecordingPdfServiceFake pdf;
    Fakes::RecordingTextSearchServiceFake textSearch;
    Fakes::RecordingShellServiceFake shell;
    Fakes::ProjectRegistryRepositoryFake projectRegistry{8U};
    take(projectRegistry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        projectId,
        "Adapter project",
        std::optional<std::string>{"adapter-project"},
        {root}}));
    Fakes::RecordingProjectMemoryService projectMemory;
    Fakes::RecordingContinuityCoordinator continuity;
    PassiveContinuityCodec continuityCodec;
    PassiveContinuityAutomation continuityAutomation;
    Domain::LegacyContinuityStatusSummary continuityStatus;
    continuityStatus.latestId = parse<Domain::LegacyHandoffId>(
        "status-latest-handoff");
    continuityStatus.latestUpdatedAt =
        Domain::UtcTimePoint{1'700'000'000s};
    continuityStatus.resumeReady = true;
    continuityStatus.resumeId = parse<Domain::LegacyHandoffId>(
        "status-resume-handoff");
    continuityStatus.openAgentSessions = 5U;
    legacyContinuity.setStatusSummary(std::move(continuityStatus));
    const auto secondaryRoot =
        take(Domain::PathText::create("D:/workspace-secondary"));
    continuityAutomation.setSnapshot(
        Domain::ContinuityAutomationStatusSnapshot{
            true,
            50U,
            200U,
            17U,
            true,
            std::optional<std::string>{"automatic-handoff"},
            {root, secondaryRoot}});
    RecordingForgeStatusRepository forgeStatus;
    const auto firstOpenSession = parse<Domain::SessionId>(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto secondOpenSession = parse<Domain::SessionId>(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    forgeStatus.setProjection(Domain::ForgeStatusProjection{
        3U, {firstOpenSession, secondOpenSession}});
    Fakes::FakeClock clock{
        Domain::UtcTimePoint{}, Domain::MonotonicTimePoint{}};
    Fakes::SequenceUuidGenerator uuidGenerator{
        std::vector<Domain::Uuid>{}};
    const auto shellExecutable = take(Domain::PathText::create(
        "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));

    auto adapter = take(Mcp::McpToolPackAdapter::create(
        Mcp::McpToolPackDependencies{
            *catalog,
            applicationPaths,
            agentCatalog,
            agentSessions,
            reportInspector,
            legacyContinuity,
            clientWorkspaceContext,
            workspaceAuthority,
            fileSystem,
            fileTextServices,
            fileTextServices,
            git,
            legacyMemory,
            pdf,
            textSearch,
            shell,
            projectRegistry,
            projectMemory,
            continuity,
            continuityCodec,
            continuityAutomation,
            forgeStatus,
            clock,
            uuidGenerator,
            Domain::ProjectMemoryLimits{},
            std::chrono::seconds{37},
            shellExecutable,
            "0.9.0",
            "windows-cpp",
            42U}));
    REQUIRE(adapter->tools().size() == 53U);

    const auto authorizeFor = [&] (
                                  const std::string& toolName,
                                  const Domain::ToolEffect effect,
                                  const std::string& canonicalArguments,
                                  const std::string& requestId,
                                  const Contracts::WorkspaceAuthority&
                                      selectedAuthority) {
        Fakes::DeterministicToolAuthorizerFake authorizer{
            toolName, effect, Domain::MonotonicTimePoint{}};
        Domain::ToolCallRequest request{
            Domain::McpRequestMetadata{
                parse<Domain::RequestId>(requestId),
                correlationId,
                clientId,
                projectId,
                "2025-06-18"},
            toolName,
            canonicalArguments};
        return take(authorizer.authorize(
            Domain::ToolAuthorizationRequest{
                request,
                effect,
                Domain::AuthorityReference{
                    selectedAuthority.authorityId(),
                    selectedAuthority.generation()}},
            selectedAuthority,
            context));
    };
    const auto authorize = [&] (
                               const std::string& toolName,
                               const Domain::ToolEffect effect,
                               const std::string& canonicalArguments,
                               const std::string& requestId) {
        return authorizeFor(
            toolName,
            effect,
            canonicalArguments,
            requestId,
            authority);
    };

    auto forgeStatusCall = authorize(
        "forge_status",
        Domain::ToolEffect::Read,
        "{}",
        "request-forge-status");
    auto forgeStatusResult = adapter->handle(
        forgeStatusCall, authority, context);
    REQUIRE(forgeStatusResult);
    const auto forgeStatusPayload = Json::parse(
        forgeStatusResult.value().canonicalPayload);
    REQUIRE(forgeStatusPayload.at("ok") == true);
    REQUIRE(forgeStatusPayload.at("home") == root.value());
    REQUIRE(forgeStatusPayload.at("presence_count") == 3U);
    REQUIRE(forgeStatusPayload.at("open_sessions") == 2U);
    REQUIRE(forgeStatusPayload.at("open_session_ids") == Json::array(
        {firstOpenSession.value(), secondOpenSession.value()}));
    REQUIRE((forgeStatusPayload.at("continuity") == Json{
        {"latest_id", "status-latest-handoff"},
        {"latest_updated_at", "2023-11-14T22:13:20.000Z"},
        {"resume_ready", true},
        {"resume_id", "status-resume-handoff"},
        {"open_agent_sessions", 5U},
        {"tools",
         Json::array({
             "session_checkpoint",
             "session_handoff",
             "context_get",
             "context_list"})},
        {"note",
         "New chat bootstrap: call context_get over stdio MCP (forge-conductor)."},
        {"auto",
         Json{
             {"checkpoint_every_tools", 50U},
             {"handoff_every_tools", 200U},
             {"note",
              "Forge writes checkpoints and handoffs from tool progress; the model does not have to call session_*."}}}}));
    REQUIRE((forgeStatusPayload.at("auto_continuity") == Json{
        {"enabled", true},
        {"checkpoint_every_tools", 50U},
        {"handoff_every_tools", 200U},
        {"progress_count", 17U},
        {"blocked", true},
        {"handoff_id", "automatic-handoff"},
        {"implicit_roots",
         Json::array({root.value(), secondaryRoot.value()})}}));
    REQUIRE(forgeStatus.calls() == 1U);

    forgeStatus.setFailure(
        std::string{Domain::ErrorCodes::DatabaseBusy});
    auto failedForgeStatusCall = authorize(
        "forge_status",
        Domain::ToolEffect::Read,
        "{}",
        "request-forge-status-failure");
    auto failedForgeStatus = adapter->handle(
        failedForgeStatusCall, authority, context);
    REQUIRE(!failedForgeStatus);
    REQUIRE(failedForgeStatus.error().code ==
            Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(forgeStatus.calls() == 2U);
    forgeStatus.setProjection(Domain::ForgeStatusProjection{
        3U, {firstOpenSession, secondOpenSession}});

    const auto handoffId = parse<Domain::LegacyHandoffId>(
        "recovered-adapter-context");
    Domain::LegacyHandoffPacket recoveredPacket{
        handoffId,
        Domain::LegacyContinuityLimits::SchemaVersion,
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{},
        Domain::LegacyHandoffSource::Model,
        true,
        std::nullopt,
        clientId,
        "Continue the adapter test",
        "ready_for_new_chat",
        std::nullopt,
        root.value(),
        {},
        {"Run the recovered tool"},
        {"D:/workspace/recovered.cpp"},
        {},
        {},
        "Recovered context",
        "Resume the adapter test.",
        false};
    Domain::LegacyContinuityRecord recoveredRecord{
        recoveredPacket, 7U, {}};
    legacyContinuity.setGetOutcome(
        Domain::LegacyContinuityGetOutcome{recoveredRecord, false});
    clientWorkspaceContext.setAdoption(Domain::ClientWorkspaceAdoption{
        Domain::ClientWorkspaceSnapshot{
            clientId,
            projectId,
            root,
            handoffId,
            recoveredRecord.writeSequence,
            12U},
        std::nullopt,
        false});
    auto recoveryAutomation = continuityAutomation.snapshot(clientId);
    recoveryAutomation.blocked = true;
    recoveryAutomation.handoffId = handoffId.value();
    continuityAutomation.setSnapshot(std::move(recoveryAutomation));

    auto contextGetCall = authorize(
        "context_get",
        Domain::ToolEffect::Read,
        "{}",
        "request-context-get");
    auto contextGetResult = adapter->handle(
        contextGetCall, authority, context);
    REQUIRE(contextGetResult);
    REQUIRE(contextGetResult.value().contextRecovery.has_value());
    REQUIRE(contextGetResult.value().contextRecovery->clientId == clientId);
    REQUIRE(contextGetResult.value().contextRecovery->handoffId == handoffId);
    REQUIRE(contextGetResult.value().contextRecovery->workingDirectory ==
            std::optional<Domain::PathText>{root});
    REQUIRE(contextGetResult.value().contextRecovery->keyFiles ==
            std::vector<Domain::PathText>{take(Domain::PathText::create(
                "D:/workspace/recovered.cpp"))});
    REQUIRE(!contextGetResult.value().continuityObservation.has_value());
    const auto contextPayload = Json::parse(
        contextGetResult.value().canonicalPayload);
    REQUIRE(contextPayload.at("found") == true);
    REQUIRE(contextPayload.at("workspace_adopted") == root.value());
    REQUIRE(contextPayload.at("workspace_project_id") == projectId.value());
    REQUIRE(contextPayload.at("projection_checked") == false);
    REQUIRE(contextPayload.at("projection_ok").is_null());
    REQUIRE(contextPayload.at("paths").empty());
    REQUIRE(!contextPayload.contains("context_budget_cleared"));
    REQUIRE(clientWorkspaceContext.adoptCalls() == 1U);
    REQUIRE(clientWorkspaceContext.lastClientId() == clientId);
    REQUIRE(clientWorkspaceContext.lastHandoffId() == handoffId);

    clientWorkspaceContext.setAdoption(Domain::ClientWorkspaceAdoption{
        Domain::ClientWorkspaceSnapshot{
            clientId,
            projectId,
            root,
            parse<Domain::LegacyHandoffId>("newer-adapter-context"),
            recoveredRecord.writeSequence + 1U,
            13U},
        Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "A newer recovered workspace superseded this adoption.",
            true),
        true});
    auto supersededContextCall = authorize(
        "context_get",
        Domain::ToolEffect::Read,
        "{}",
        "request-context-get-superseded");
    auto supersededContextResult = adapter->handle(
        supersededContextCall, authority, context);
    REQUIRE(!supersededContextResult);
    REQUIRE(supersededContextResult.error().code ==
            Domain::ErrorCodes::Conflict);
    REQUIRE(supersededContextResult.error().retryable);
    REQUIRE(clientWorkspaceContext.adoptCalls() == 2U);

    legacyContinuity.setGetOutcome(
        Domain::LegacyContinuityGetOutcome{std::nullopt, false});
    auto missingContextCall = authorize(
        "context_get",
        Domain::ToolEffect::Read,
        "{}",
        "request-context-get-missing");
    auto missingContextResult = adapter->handle(
        missingContextCall, authority, context);
    REQUIRE(missingContextResult);
    REQUIRE(!missingContextResult.value().contextRecovery.has_value());
    const auto missingContextPayload = Json::parse(
        missingContextResult.value().canonicalPayload);
    REQUIRE(missingContextPayload.at("found") == false);
    REQUIRE(!missingContextPayload.contains("context_budget_cleared"));
    REQUIRE(clientWorkspaceContext.adoptCalls() == 2U);

    const std::string fileText = "alpha\nbeta\ngamma";
    std::vector<std::byte> fileBytes;
    fileBytes.reserve(fileText.size());
    for (const auto value : fileText) {
        fileBytes.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    fileSystem.readFileResult.set(
        Domain::Result<std::vector<std::byte>>::success(std::move(fileBytes)));
    auto readCall = authorize(
        "fs_read",
        Domain::ToolEffect::Read,
        R"({"limit":"1","offset":"2","path":"notes.txt"})",
        "request-fs-read");
    auto readResult = adapter->handle(readCall, authority, context);
    REQUIRE(readResult);
    REQUIRE(readResult.value().receipt.ok);
    REQUIRE(readResult.value().continuityObservation.has_value());
    REQUIRE(readResult.value().continuityObservation->path.has_value());
    REQUIRE(readResult.value().continuityObservation->path->value() ==
            "D:/workspace\\notes.txt");
    REQUIRE(!readResult.value().continuityObservation->workingDirectory);
    REQUIRE(readResult.value().continuityObservation->baseDirectory ==
            std::optional<Domain::PathText>{root});
    const auto readPayload = Json::parse(readResult.value().canonicalPayload);
    REQUIRE(readPayload.at("content") == "beta");
    REQUIRE(readPayload.at("start_line") == 2);
    REQUIRE(readPayload.at("end_line") == 2);
    REQUIRE(readPayload.at("line_count") == 1);
    REQUIRE(readPayload.at("next_offset") == 3);
    REQUIRE(readPayload.at("has_more") == true);
    REQUIRE(fileSystem.calls() == 1U);
    REQUIRE(fileSystem.lastCapture().has_value());
    REQUIRE(fileSystem.lastCapture()->primary.canonicalPath().value() ==
            "D:/workspace\\notes.txt");

    const auto agentWorkingDirectory = take(Domain::PathText::create(
        "D:/workspace\\agent-work"));
    const auto agentId = parse<Domain::AgentId>("explore");
    const auto agentSessionId = parse<Domain::SessionId>(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const Domain::AgentSession agentSession{
        agentSessionId,
        agentId,
        clientId,
        Domain::SessionStatus::Open,
        std::nullopt,
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{}};
    const Domain::AgentRunRecord agentRun{
        agentSession,
        projectId,
        std::optional<std::string>{"Inspect the authorized workspace"},
        agentWorkingDirectory,
        {"report"},
        {"Inspect files"},
        std::nullopt};
    const Domain::AgentSpec agentSpec{
        agentId,
        "Explore",
        "Inspect the workspace",
        {"fs_read"},
        {},
        {"Workspace inspection"},
        {"Inspect files"},
        {"Report findings"},
        {"report"},
        {},
        {},
        "Inspect the authorized workspace.",
        "builtin"};
    agentSessions.startRunResult.set(
        Domain::Result<Domain::AgentRunStartOutcome>::success(
            Domain::AgentRunStartOutcome{
                agentRun, std::nullopt, agentSpec, 0U, true}));
    auto agentStartCall = authorize(
        "agent_run_start",
        Domain::ToolEffect::Write,
        R"({"agent_id":"explore","cwd":"agent-work","goal":"Inspect the authorized workspace"})",
        "request-agent-start-authorized-cwd");
    auto agentStart = adapter->handle(agentStartCall, authority, context);
    REQUIRE(agentStart);
    REQUIRE(agentStart.value().receipt.ok);
    REQUIRE(agentStart.value().continuityObservation.has_value());
    REQUIRE(!agentStart.value().continuityObservation->path);
    REQUIRE(agentStart.value().continuityObservation->workingDirectory ==
            std::optional<Domain::PathText>{agentWorkingDirectory});
    REQUIRE(agentStart.value().continuityObservation->baseDirectory ==
            std::optional<Domain::PathText>{root});
    REQUIRE(agentSessions.lastCapture().has_value());
    REQUIRE(agentSessions.lastCapture()->startRequest.has_value());
    REQUIRE(agentSessions.lastCapture()->startRequest->workingDirectory ==
            std::optional<Domain::PathText>{agentWorkingDirectory});

    const auto agentCallsBeforeRejectedCwd = agentSessions.calls();
    auto rejectedAgentStartCall = authorize(
        "agent_run_start",
        Domain::ToolEffect::Write,
        R"({"agent_id":"explore","cwd":"D:/outside","goal":"Reject this cwd"})",
        "request-agent-start-rejected-cwd");
    auto rejectedAgentStart = adapter->handle(
        rejectedAgentStartCall, authority, context);
    REQUIRE(!rejectedAgentStart);
    REQUIRE(rejectedAgentStart.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(agentSessions.calls() == agentCallsBeforeRejectedCwd);

    std::vector<std::byte> largeFile(
        1'100'000U,
        static_cast<std::byte>(static_cast<unsigned char>('x')));
    fileSystem.readFileResult.set(
        Domain::Result<std::vector<std::byte>>::success(
            std::move(largeFile)));
    auto largeReadCall = authorize(
        "fs_read",
        Domain::ToolEffect::Read,
        R"({"path":"large-line.txt"})",
        "request-fs-read-large");
    auto largeReadResult = adapter->handle(
        largeReadCall, authority, context);
    REQUIRE(largeReadResult);
    REQUIRE(largeReadResult.value().canonicalPayload.size() <
            Mcp::McpJsonCodec::MaximumDocumentBytes);
    const auto largeReadPayload = Json::parse(
        largeReadResult.value().canonicalPayload);
    REQUIRE(largeReadPayload.at("has_more") == true);
    REQUIRE(largeReadPayload.at("next_offset").is_null());
    REQUIRE(largeReadPayload.at("next_byte_offset").is_number_unsigned());
    REQUIRE(largeReadPayload.at("content").get<std::string>().size() <=
            96U * 1024U);
    const auto nextByteOffset =
        largeReadPayload.at("next_byte_offset").get<std::size_t>();
    auto continuedReadCall = authorize(
        "fs_read",
        Domain::ToolEffect::Read,
        Json{
            {"byte_offset", nextByteOffset},
            {"path", "large-line.txt"}}
            .dump(),
        "request-fs-read-large-continued");
    auto continuedReadResult = adapter->handle(
        continuedReadCall, authority, context);
    REQUIRE(continuedReadResult);
    const auto continuedPayload = Json::parse(
        continuedReadResult.value().canonicalPayload);
    REQUIRE(continuedPayload.at("byte_offset") == nextByteOffset);
    REQUIRE(continuedPayload.at("content").get<std::string>().size() <=
            96U * 1024U);

    auto closedSchemaCall = authorize(
        "project_memory.status",
        Domain::ToolEffect::Read,
        "{\"project_id\":\"" + projectId.value() +
            "\",\"unknown_field\":true}",
        "request-closed-schema");
    auto closedSchemaResult = adapter->handle(
        closedSchemaCall, authority, context);
    REQUIRE(!closedSchemaResult);
    REQUIRE(closedSchemaResult.error().code ==
            Domain::ErrorCodes::InvalidRequest);
    REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Status) == 0U);

    const auto memoryRecordId = parse<Domain::MemoryRecordId>(
        "90000000-0000-4000-8000-000000000009");
    const auto memoryDigest = parse<Domain::Sha256Digest>(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const Domain::MemoryWriteOutcome memoryWriteOutcome{
        projectId,
        memoryRecordId,
        3U,
        Domain::MemoryWriteDisposition::Inserted,
        memoryDigest,
        Domain::ProjectMemorySchemaVersion,
        Domain::ProjectMemoryCapabilityVersion};

    projectMemory.initializeResult.set(
        Domain::Result<Domain::ProjectInitialization>::success(
            Domain::ProjectInitialization{
                Domain::ProjectMemoryDescriptor{
                    projectId,
                    "Adapter project",
                    std::optional<std::string>{"adapter-project"},
                    {root}},
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion,
                Domain::ProjectMemoryLimits{},
                true,
                true,
                true}));
    auto initializeAliasCall = authorize(
        "project_memory.initialize",
        Domain::ToolEffect::Write,
        Json{
            {"path", " \tD:/workspace\r\n"},
            {"display_name", 2026},
            {"idempotency_key", 31415},
            {"deadline_ms", "5000"}}
            .dump(),
        "request-project-memory-initialize-alias");
    auto initializeAlias = adapter->handle(
        initializeAliasCall, authority, context);
    REQUIRE(initializeAlias);
    REQUIRE(Json::parse(initializeAlias.value().canonicalPayload).at(
                "project_id") == projectId.value());
    REQUIRE(projectMemory.callCount(
                Fakes::ProjectMemoryCall::Initialize) == 1U);

    projectMemory.searchResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            projectId,
            {},
            std::nullopt,
            false,
            0U,
            2'048U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    auto normalizedSearchCall = authorize(
        "project_memory.search",
        Domain::ToolEffect::Read,
        Json{
            {"project_id", "  " + projectId.value() + "\n"},
            {"query", 17},
            {"kinds", "decision"},
            {"tags", Json::array({"alpha", 9, "beta"})},
            {"limit", "2"},
            {"maximum_response_bytes", 2048.0},
            {"include_body", "false"}}
            .dump(),
        "request-project-memory-normalized-search");
    auto normalizedSearch = adapter->handle(
        normalizedSearchCall, authority, context);
    REQUIRE(normalizedSearch);
    REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Search) == 1U);
    const auto normalizedSearchPayload = Json::parse(
        normalizedSearch.value().canonicalPayload);
    REQUIRE(normalizedSearchPayload.at("project_id") == projectId.value());
    REQUIRE(normalizedSearchPayload.at("records") == Json::array());

    projectMemory.rememberBatchResult.set(
        Domain::Result<Domain::MemoryBatchOutcome>::success(
            Domain::MemoryBatchOutcome{
                projectId,
                {memoryWriteOutcome},
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion}));
    auto normalizedBatchCall = authorize(
        "project_memory.remember_batch",
        Domain::ToolEffect::Write,
        Json{
            {"project_id", projectId.value()},
            {"deadline_ms", "5000"},
            {"items",
             Json::array({Json{
                 {"kind", " decision "},
                 {"title", 123},
                 {"summary", " Batch summary "},
                 {"body", Json::object()},
                 {"tags", "batch-tag"},
                 {"importance", "default-me"},
                 {"confidence", false},
                 {"source_reference", 99},
                 {"related_ids",
                  Json::array({memoryRecordId.value(), 7})}}})}}
            .dump(),
        "request-project-memory-normalized-batch");
    auto normalizedBatch = adapter->handle(
        normalizedBatchCall, authority, context);
    REQUIRE(normalizedBatch);
    REQUIRE(projectMemory.callCount(
                Fakes::ProjectMemoryCall::RememberBatch) == 1U);
    const auto normalizedBatchPayload = Json::parse(
        normalizedBatch.value().canonicalPayload);
    REQUIRE(normalizedBatchPayload.at("count") == 1U);
    REQUIRE(normalizedBatchPayload.at("results").at(0).at("record_id") ==
            memoryRecordId.value());

    projectMemory.importResult.set(
        Domain::Result<Domain::ProjectMemoryImport>::success(
            Domain::ProjectMemoryImport{
                projectId,
                Domain::ImportDisposition::Preview,
                4U,
                2U,
                memoryDigest,
                {}}));
    auto previewImportCall = authorize(
        "project_memory.import",
        Domain::ToolEffect::Write,
        Json{
            {"project_id", projectId.value()},
            {"artifact", " memory-export.json "},
            {"preview", "not-a-bool"}}
            .dump(),
        "request-project-memory-preview-import");
    auto previewImport = adapter->handle(
        previewImportCall, authority, context);
    REQUIRE(previewImport);
    const auto previewImportPayload = Json::parse(
        previewImport.value().canonicalPayload);
    REQUIRE(previewImportPayload.at("preview") == true);
    REQUIRE(previewImportPayload.at("disposition") == "preview");
    REQUIRE(previewImportPayload.at("record_count") == 4U);
    REQUIRE(previewImportPayload.contains("imported"));
    REQUIRE(!previewImportPayload.contains("results"));

    projectMemory.importResult.set(
        Domain::Result<Domain::ProjectMemoryImport>::success(
            Domain::ProjectMemoryImport{
                projectId,
                Domain::ImportDisposition::Imported,
                1U,
                1U,
                memoryDigest,
                {memoryWriteOutcome}}));
    auto committedImportCall = authorize(
        "project_memory.import",
        Domain::ToolEffect::Write,
        Json{
            {"project_id", projectId.value()},
            {"artifact", "memory-export.json"},
            {"preview", false}}
            .dump(),
        "request-project-memory-committed-import");
    auto committedImport = adapter->handle(
        committedImportCall, authority, context);
    REQUIRE(committedImport);
    const auto committedImportPayload = Json::parse(
        committedImport.value().canonicalPayload);
    REQUIRE(committedImportPayload.size() == 6U);
    REQUIRE(committedImportPayload.at("ok") == true);
    REQUIRE(committedImportPayload.at("project_id") == projectId.value());
    REQUIRE(committedImportPayload.at("count") == 1U);
    REQUIRE(committedImportPayload.at("results").at(0).at("record_id") ==
            memoryRecordId.value());
    REQUIRE(committedImportPayload.at("schema_version") ==
            Domain::ProjectMemorySchemaVersion);
    REQUIRE(committedImportPayload.at("capability_version") ==
            Domain::ProjectMemoryCapabilityVersion);
    REQUIRE(!committedImportPayload.contains("preview"));
    REQUIRE(!committedImportPayload.contains("disposition"));

    continuity.statusResult.set(
        Domain::Result<Domain::ContinuityStatus>::success(
            Domain::ContinuityStatus{
                projectId, std::nullopt, 0U, 0U, false}));
    auto inactiveContinuityStatusCall = authorize(
        "continuity.status",
        Domain::ToolEffect::Read,
        Json{{"project_id", " \t" + projectId.value() + "\r\n"}}.dump(),
        "request-continuity-inactive-status");
    auto inactiveContinuityStatus = adapter->handle(
        inactiveContinuityStatusCall, authority, context);
    REQUIRE(inactiveContinuityStatus);
    const auto inactiveContinuityPayload = Json::parse(
        inactiveContinuityStatus.value().canonicalPayload);
    REQUIRE(inactiveContinuityPayload.at("state") == "active");
    REQUIRE(inactiveContinuityPayload.at("operation").is_null());

    const auto verifyReadFailure = [&] (
        Domain::Error error,
        const std::string_view expectedCode,
        const std::string& requestId) {
        fileSystem.readFileResult.set(
            Domain::Result<std::vector<std::byte>>::failure(
                std::move(error)));
        auto call = authorize(
            "fs_read",
            Domain::ToolEffect::Read,
            R"({"path":"failure.txt"})",
            requestId);
        auto result = adapter->handle(call, authority, context);
        REQUIRE(!result);
        REQUIRE(result.error().code == expectedCode);
        return result.error();
    };
    auto oversizedReadError = verifyReadFailure(
        Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The native text file exceeds its byte bound.",
            true,
            std::optional<std::string>{"read-evidence"}),
        "file_too_large",
        "request-fs-read-payload-remap");
    REQUIRE(oversizedReadError.retryable);
    REQUIRE(oversizedReadError.evidenceId ==
            std::optional<std::string>{"read-evidence"});
    (void)verifyReadFailure(
        Domain::makeError(
            Domain::ErrorCodes::RecordNotFound,
            "The native text file was not found."),
        "not_found",
        "request-fs-read-missing-remap");
    (void)verifyReadFailure(
        Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Text is not valid UTF-8."),
        "not_found",
        "request-fs-read-utf8-remap");
    (void)verifyReadFailure(
        Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            "The native text file is not authorized."),
        Domain::ErrorCodes::Unauthorized,
        "request-fs-read-unauthorized-preserved");

    const auto verifyEditFailure = [&] (
        Domain::Error error,
        const std::string_view expectedCode,
        const std::string& requestId) {
        fileTextServices.setReplaceAllResult(
            Domain::Result<Contracts::TextFileEditReport>::failure(
                std::move(error)));
        auto call = authorize(
            "fs_edit",
            Domain::ToolEffect::Write,
            R"({"path":"failure.txt","old":"before","new":"after"})",
            requestId);
        auto result = adapter->handle(call, authority, context);
        REQUIRE(!result);
        REQUIRE(result.error().code == expectedCode);
    };
    verifyEditFailure(
        Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The edited text would exceed 2 MiB."),
        "file_too_large",
        "request-fs-edit-payload-remap");
    verifyEditFailure(
        Domain::makeError(
            Domain::ErrorCodes::RecordNotFound,
            "The native text file was not found."),
        "not_found",
        "request-fs-edit-missing-remap");
    verifyEditFailure(
        Domain::makeError(
            Domain::ErrorCodes::RecordNotFound,
            "The text-edit search value was not found."),
        "no_match",
        "request-fs-edit-no-match-remap");

    const auto verifyPdfSourceFailure = [&] (
        Domain::Error error,
        const std::string& requestId) {
        pdf.fromTextFileResult.set(
            Domain::Result<Domain::PdfWriteReceipt>::failure(
                std::move(error)));
        auto call = authorize(
            "pdf_from_file",
            Domain::ToolEffect::Write,
            R"({"source_path":"source.md"})",
            requestId);
        auto result = adapter->handle(call, authority, context);
        REQUIRE(!result);
        REQUIRE(result.error().code == "not_found");
    };
    verifyPdfSourceFailure(
        Domain::makeError(
            Domain::ErrorCodes::RecordNotFound,
            "The PDF source file was not found."),
        "request-pdf-source-missing-remap");
    verifyPdfSourceFailure(
        Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The PDF source file must contain valid NUL-free UTF-8 text."),
        "request-pdf-source-utf8-remap");

    const auto defaultPdfPath = take(Domain::PathText::create(
        "D:/workspace\\source.pdf"));
    pdf.fromTextFileResult.set(
        Domain::Result<Domain::PdfWriteReceipt>::success(
            Domain::PdfWriteReceipt{
                defaultPdfPath, 128U, 1U, "test-pdf", "source"}));
    auto emptyPdfDestinationCall = authorize(
        "pdf_from_file",
        Domain::ToolEffect::Write,
        R"({"source_path":"source.md","dest_path":""})",
        "request-pdf-empty-destination-default");
    auto emptyPdfDestination = adapter->handle(
        emptyPdfDestinationCall, authority, context);
    REQUIRE(emptyPdfDestination);
    REQUIRE(pdf.lastCapture().has_value());
    REQUIRE(pdf.lastCapture()->secondary.has_value());
    REQUIRE(pdf.lastCapture()->secondary->canonicalPath() == defaultPdfPath);

    textSearch.searchResult.set(
        Domain::Result<std::vector<std::string>>::success(
            {"D:/workspace/file.cpp:1:2026"}));
    auto numericSearchPatternCall = authorize(
        "search_text",
        Domain::ToolEffect::Read,
        R"({"pattern":2026,"path":"D:/workspace"})",
        "request-search-numeric-pattern");
    auto numericSearchPattern = adapter->handle(
        numericSearchPatternCall, authority, context);
    REQUIRE(numericSearchPattern);
    REQUIRE(textSearch.lastCapture().has_value());
    REQUIRE(textSearch.lastCapture()->query == "2026");

    fileSystem.createDirectoryResult.set(Domain::Result<void>::success());
    auto mkdirCall = authorize(
        "fs_mkdir",
        Domain::ToolEffect::Write,
        R"({"path":"created-directory"})",
        "request-fs-mkdir");
    auto mkdirResult = adapter->handle(mkdirCall, authority, context);
    REQUIRE(mkdirResult);
    const auto mkdirPayload = Json::parse(
        mkdirResult.value().canonicalPayload);
    REQUIRE(mkdirPayload.at("created") == true);
    REQUIRE(!mkdirPayload.contains("deleted"));

    const std::string idempotencyKey = "caller-checkpoint-key";
    auto checkpointCall = authorize(
        "continuity.checkpoint",
        Domain::ToolEffect::Write,
        Json{
            {"handoff_id", "50000000-0000-4000-8000-000000000005"},
            {"idempotency_key", idempotencyKey},
            {"mission", "Preserve caller idempotency"},
            {"operation_id", "40000000-0000-4000-8000-000000000004"},
            {"predecessor_session_id",
             "60000000-0000-4000-8000-000000000006"},
            {"project_id", projectId.value()}}
            .dump(),
        "request-continuity-checkpoint");
    const auto checkpointResult = adapter->handle(
        checkpointCall, authority, context);
    REQUIRE(!checkpointResult);
    REQUIRE(checkpointResult.error().code ==
            Domain::ErrorCodes::InternalFailure);
    REQUIRE(continuity.callCount(Fakes::ContinuityCall::Checkpoint) == 1U);
    REQUIRE(continuity.lastCheckpointRequest().has_value());
    REQUIRE(continuity.lastCheckpointRequest()->idempotencyKey.has_value());
    REQUIRE(continuity.lastCheckpointRequest()->idempotencyKey->value() ==
            idempotencyKey);

    auto normalizedCheckpointCall = authorize(
        "continuity.checkpoint",
        Domain::ToolEffect::Write,
        Json{
            {"project_id", " \t" + projectId.value() + "\r\n"},
            {"operation_id", " 40000000-0000-4000-8000-000000000004 "},
            {"handoff_id", " 50000000-0000-4000-8000-000000000005 "},
            {"predecessor_session_id",
             " 60000000-0000-4000-8000-000000000006 "},
            {"mission", 42},
            {"repository_root", " \tD:/workspace\r\n"},
            {"branch", 17},
            {"constraints", "scalar-is-not-a-continuity-list"},
            {"dirty_summary", Json::array({"first", 9, "second"})},
            {"active_files",
             Json::array({"D:/workspace/file.cpp", false})},
            {"open_work", Json::array({"remaining", Json::object()})},
            {"next_actions", "scalar-defaults-to-empty"}}
            .dump(),
        "request-continuity-normalized-checkpoint");
    auto normalizedCheckpoint = adapter->handle(
        normalizedCheckpointCall, authority, context);
    REQUIRE(!normalizedCheckpoint);
    REQUIRE(normalizedCheckpoint.error().code ==
            Domain::ErrorCodes::InternalFailure);
    REQUIRE(continuity.callCount(Fakes::ContinuityCall::Checkpoint) == 2U);
    REQUIRE(continuity.lastCheckpointRequest().has_value());
    const auto& normalizedHandoff =
        continuity.lastCheckpointRequest()->handoff;
    REQUIRE(normalizedHandoff.mission == "42");
    REQUIRE(normalizedHandoff.project.repositoryRoot == root);
    REQUIRE(normalizedHandoff.project.branch == "17");
    REQUIRE(normalizedHandoff.constraints.empty());
    REQUIRE(normalizedHandoff.project.dirtySummary ==
            std::vector<std::string>({"first", "second"}));
    REQUIRE(normalizedHandoff.currentWork.activeFiles ==
            std::vector<Domain::PathText>({
                take(Domain::PathText::create("D:/workspace/file.cpp"))}));
    REQUIRE(normalizedHandoff.openWork.size() == 1U);
    REQUIRE(normalizedHandoff.openWork.at(0).summary == "remaining");
    REQUIRE(normalizedHandoff.nextActions.size() == 1U);
    REQUIRE(normalizedHandoff.nextActions.at(0).action ==
            "Continue current work");

    git.statusResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0, "", "", false, false,
            false, false, true, 1ms}));
    auto defaultedGitCall = authorize(
        "git_status",
        Domain::ToolEffect::Read,
        "{}",
        "request-git-defaulted-cwd");
    auto defaultedGit = adapter->handle(
        defaultedGitCall, authority, context);
    REQUIRE(defaultedGit);
    REQUIRE(defaultedGit.value().receipt.ok);
    REQUIRE(defaultedGit.value().continuityObservation.has_value());
    REQUIRE(!defaultedGit.value().continuityObservation->path);
    REQUIRE(defaultedGit.value().continuityObservation->workingDirectory ==
            std::optional<Domain::PathText>{root});
    REQUIRE(defaultedGit.value().continuityObservation->baseDirectory ==
            std::optional<Domain::PathText>{root});

    git.statusResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            17, "partial status", "fatal status", false, false,
            false, false, true, 3ms}));
    auto gitFailureCall = authorize(
        "git_status",
        Domain::ToolEffect::Read,
        R"({"cwd":"D:/workspace"})",
        "request-git-nonzero");
    auto gitFailure = adapter->handle(
        gitFailureCall, authority, context);
    REQUIRE(gitFailure);
    REQUIRE(!gitFailure.value().receipt.ok);
    REQUIRE(gitFailure.value().receipt.error.has_value());
    REQUIRE(gitFailure.value().receipt.error->code ==
            Domain::ErrorCodes::ProcessExitNonzero);
    REQUIRE(gitFailure.value().continuityObservation.has_value());
    REQUIRE(gitFailure.value().continuityObservation->workingDirectory ==
            std::optional<Domain::PathText>{root});
    REQUIRE(Json::parse(gitFailure.value().canonicalPayload).at("stdout") ==
            "partial status");

    const auto verifyShellFailure = [&] (
        Domain::ProcessResult process,
        const std::string_view expectedCode,
        const std::string& requestId) {
        shell.executeResult.set(
            Domain::Result<Domain::ProcessResult>::success(
                std::move(process)));
        auto call = authorizeFor(
            "shell_exec",
            Domain::ToolEffect::Write,
            R"({"command":"Write-Output test","cwd":"D:/workspace"})",
            requestId,
            shellAuthority);
        auto result = adapter->handle(
            call, shellAuthority, context);
        REQUIRE(result);
        REQUIRE(!result.value().receipt.ok);
        REQUIRE(result.value().receipt.error.has_value());
        REQUIRE(result.value().receipt.error->code == expectedCode);
        REQUIRE(result.value().continuityObservation.has_value());
        REQUIRE(result.value().continuityObservation->workingDirectory ==
                std::optional<Domain::PathText>{root});
        REQUIRE(shell.lastExecution().has_value());
        REQUIRE(shell.lastExecution()->request.has_value());
        REQUIRE(shell.lastExecution()->request->timeout == 37s);
        REQUIRE(Json::parse(result.value().canonicalPayload).at("ok") ==
                false);
    };
    verifyShellFailure(
        Domain::ProcessResult{
            9, "partial shell", "failure", false, false,
            false, false, true, 4ms},
        Domain::ErrorCodes::ProcessExitNonzero,
        "request-shell-nonzero");
    verifyShellFailure(
        Domain::ProcessResult{
            0, "", "", true, false, false, false, true, 30s},
        Domain::ErrorCodes::ProcessTimeout,
        "request-shell-timeout");
    verifyShellFailure(
        Domain::ProcessResult{
            0, "", "", false, true, false, false, true, 5ms},
        Domain::ErrorCodes::Cancelled,
        "request-shell-cancelled");
    verifyShellFailure(
        Domain::ProcessResult{
            0, "", "", false, false, false, false, false, 5ms},
        Domain::ErrorCodes::ProcessTerminationUnconfirmed,
        "request-shell-unconfirmed");
}

void testRealRouterContinuityIntegration()
{
    const auto authorityId = parse<Domain::AuthorityId>(
        "70000000-0000-4000-8000-000000000007");
    const auto projectId = parse<Domain::ProjectId>(
        "71000000-0000-4000-8000-000000000007");
    const auto clientId = parse<Domain::ClientId>("integration-client");
    const auto root = take(Domain::PathText::create("D:/workspace"));
    const auto recoveredWorkingDirectory = take(Domain::PathText::create(
        "D:/recovered/project"));
    const auto recoveredKeyFile = take(Domain::PathText::create(
        "D:/recovered/project/src/main.cpp"));
    const auto recoveredKeyFileRoot = take(Domain::PathText::create(
        "D:/recovered/project/src"));
    const auto handoffId = parse<Domain::LegacyHandoffId>(
        "integration-automatic-handoff");
    const auto staleHandoffId = parse<Domain::LegacyHandoffId>(
        "integration-stale-handoff");

    Fakes::FakeClock clock{
        Domain::UtcTimePoint{1'735'789'855s},
        Domain::MonotonicTimePoint{1s}};
    const auto authorityOperationId = parse<Domain::OperationId>(
        "72000000-0000-4000-8000-000000000007");
    const auto authorityCorrelationId = parse<Domain::CorrelationId>(
        "integration-authority");
    const Domain::OperationContext authorityContext{
        authorityOperationId,
        clock.monotonicNow() + 5min,
        {},
        authorityCorrelationId};
    Fakes::DeterministicWorkspaceAuthority workspaceAuthority{
        authorityId,
        clientId,
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        {},
        true,
        17U};
    auto authority = take(workspaceAuthority.authorityFor(
        projectId, authorityContext));

    Domain::LegacyHandoffPacket recoveredPacket{
        handoffId,
        Domain::LegacyContinuityLimits::SchemaVersion,
        clock.utcNow(),
        clock.utcNow(),
        Domain::LegacyHandoffSource::Budget,
        true,
        std::nullopt,
        clientId,
        "Continue the router integration",
        "handoff_ready",
        std::nullopt,
        recoveredWorkingDirectory.value(),
        {},
        {"Continue from recovered context"},
        {recoveredKeyFile.value()},
        {},
        {},
        "Router integration continuity",
        "Resume the router integration.",
        false};
    Domain::LegacyContinuityRecord recoveredRecord{
        recoveredPacket, 19U, {}};

    PassiveLegacyContinuity legacyContinuity;
    legacyContinuity.setAutomaticOutcome(
        Domain::LegacyContinuityPersistOutcome{
            recoveredRecord,
            true,
            true,
            false,
            std::nullopt,
            {}});
    legacyContinuity.setGetOutcome(
        Domain::LegacyContinuityGetOutcome{recoveredRecord, true});
    Domain::LegacyContinuityStatusSummary continuityStatus;
    continuityStatus.latestId = handoffId;
    continuityStatus.resumeReady = true;
    continuityStatus.resumeId = handoffId;
    legacyContinuity.setStatusSummary(std::move(continuityStatus));

    Fakes::ScriptedHasher hasher{parse<Domain::Sha256Digest>(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")};
    auto guard = take(Mcp::McpInvocationGuard::create(
        legacyContinuity,
        hasher,
        clock,
        Mcp::McpInvocationGuardPolicy{
            50U,
            100U,
            1U,
            1U,
            3'600U,
            3'600U}));

    auto catalog = take(Mcp::McpToolCatalog::create());
    Fakes::RecordingApplicationPathsFake applicationPaths;
    applicationPaths.dataRootResult.set(
        Domain::Result<Domain::PathText>::success(root));
    Fakes::RecordingAgentCatalogFake agentCatalog;
    agentCatalog.allResult.set(
        Domain::Result<std::vector<Domain::AgentSpec>>::success({}));
    Fakes::RecordingAgentSessionServiceFake agentSessions;
    PassiveReportInspector reportInspector;
    RecordingClientWorkspaceContext clientWorkspaceContext;
    clientWorkspaceContext.setAdoption(Domain::ClientWorkspaceAdoption{
        Domain::ClientWorkspaceSnapshot{
            clientId,
            projectId,
            recoveredWorkingDirectory,
            handoffId,
            recoveredRecord.writeSequence,
            21U},
        std::nullopt,
        false});
    Fakes::RecordingFileSystemFake fileSystem{3U * 1024U * 1024U};
    const std::string progressText = "progress";
    std::vector<std::byte> progressBytes;
    progressBytes.reserve(progressText.size());
    for (const auto value : progressText) {
        progressBytes.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    fileSystem.readFileResult.set(
        Domain::Result<std::vector<std::byte>>::success(
            std::move(progressBytes)));
    PassiveFileTextServices fileTextServices;
    Fakes::RecordingGitServiceFake git;
    Fakes::LegacyMemoryServiceFake legacyMemory{
        32U,
        Domain::DestructiveConfirmation{
            "purge_legacy_memory", "all", "integration-token"}};
    Fakes::RecordingPdfServiceFake pdf;
    Fakes::RecordingTextSearchServiceFake textSearch;
    Fakes::RecordingShellServiceFake shell;
    Fakes::ProjectRegistryRepositoryFake projectRegistry{8U};
    Fakes::RecordingProjectMemoryService projectMemory;
    Fakes::RecordingContinuityCoordinator continuity;
    PassiveContinuityCodec continuityCodec;
    RecordingForgeStatusRepository forgeStatus;
    forgeStatus.setProjection(Domain::ForgeStatusProjection{1U, {}});
    Fakes::SequenceUuidGenerator uuidGenerator{
        std::vector<Domain::Uuid>{}};
    const auto shellExecutable = take(Domain::PathText::create(
        "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));

    auto adapter = take(Mcp::McpToolPackAdapter::create(
        Mcp::McpToolPackDependencies{
            *catalog,
            applicationPaths,
            agentCatalog,
            agentSessions,
            reportInspector,
            legacyContinuity,
            clientWorkspaceContext,
            workspaceAuthority,
            fileSystem,
            fileTextServices,
            fileTextServices,
            git,
            legacyMemory,
            pdf,
            textSearch,
            shell,
            projectRegistry,
            projectMemory,
            continuity,
            continuityCodec,
            *guard,
            forgeStatus,
            clock,
            uuidGenerator,
            Domain::ProjectMemoryLimits{},
            std::chrono::seconds{30},
            shellExecutable,
            "0.9.0",
            "windows-cpp",
            77U}));
    Mcp::McpToolAuthorizer authorizer{clock};
    Fakes::AuditRepositoryFake audit{32U, clock.monotonicNow()};
    std::array<Contracts::IToolHandler*, 1U> handlers{adapter.get()};
    auto router = take(Mcp::McpToolRouter::create(
        *catalog,
        handlers,
        authorizer,
        *guard,
        audit,
        hasher,
        clock));

    std::uint64_t sequence{};
    const auto invoke = [&] (
                            const std::string& toolName,
                            const std::string& arguments) {
        ++sequence;
        const auto suffix = std::to_string(sequence);
        std::string operationText{"73000000-0000-4000-8000-"};
        operationText.append(12U - suffix.size(), '0');
        operationText += suffix;
        const auto correlation = parse<Domain::CorrelationId>(
            "integration-correlation-" + suffix);
        Domain::ToolCallRequest request{
            Domain::McpRequestMetadata{
                parse<Domain::RequestId>(
                    "integration-request-" + suffix),
                correlation,
                clientId,
                projectId,
                "2025-06-18"},
            toolName,
            arguments};
        return router->invoke(
            request,
            authority,
            Domain::OperationContext{
                parse<Domain::OperationId>(operationText),
                clock.monotonicNow() + 5min,
                {},
                correlation});
    };

    auto progress = invoke("fs_read", R"({"path":"notes.txt"})");
    REQUIRE(progress);
    REQUIRE(progress.value().receipt.ok);
    const auto progressPayload = Json::parse(
        progress.value().canonicalPayload);
    REQUIRE(progressPayload.at("auto_continuity") == "handoff");
    REQUIRE(progressPayload.at("handoff_id") == handoffId.value());
    REQUIRE(progressPayload.at("handoff_required") == true);
    REQUIRE(legacyContinuity.automaticCalls() == 1U);
    REQUIRE(legacyContinuity.lastAutomaticClientId() ==
            std::optional<Domain::ClientId>{clientId});
    REQUIRE(legacyContinuity.lastAutomaticRequest().has_value());
    REQUIRE(legacyContinuity.lastAutomaticRequest()->finalize);
    REQUIRE(legacyContinuity.lastAutomaticRequest()->inferred.keyFiles ==
            std::optional<std::vector<std::string>>{
                {"D:/workspace\\notes.txt"}});

    auto blockedForgeStatus = invoke("forge_status", "{}");
    REQUIRE(blockedForgeStatus);
    const auto blockedPayload = Json::parse(
        blockedForgeStatus.value().canonicalPayload);
    const auto& blockedAutomatic = blockedPayload.at("auto_continuity");
    REQUIRE(blockedAutomatic.at("enabled") == true);
    REQUIRE(blockedAutomatic.at("checkpoint_every_tools") == 1U);
    REQUIRE(blockedAutomatic.at("handoff_every_tools") == 1U);
    REQUIRE(blockedAutomatic.at("progress_count") == 1U);
    REQUIRE(blockedAutomatic.at("blocked") == true);
    REQUIRE(blockedAutomatic.at("handoff_id") == handoffId.value());
    REQUIRE(blockedAutomatic.at("implicit_roots") ==
            Json::array({root.value()}));

    auto stalePacket = recoveredPacket;
    stalePacket.id = staleHandoffId;
    stalePacket.workingDirectory = "D:/stale/project";
    stalePacket.keyFiles = {"D:/stale/project/file.cpp"};
    legacyContinuity.setGetOutcome(
        Domain::LegacyContinuityGetOutcome{
            Domain::LegacyContinuityRecord{
                std::move(stalePacket), 18U, {}},
            true});
    auto staleRecovery = invoke(
        "context_get",
        Json{{"handoff_id", staleHandoffId.value()}}.dump());
    REQUIRE(!staleRecovery);
    REQUIRE(staleRecovery.error().code == Domain::ErrorCodes::Conflict);
    REQUIRE(clientWorkspaceContext.adoptCalls() == 0U);
    const auto afterStale = guard->snapshot(clientId);
    REQUIRE(afterStale.blocked);
    REQUIRE(afterStale.handoffId ==
            std::optional<std::string>{handoffId.value()});
    REQUIRE(afterStale.implicitRoots ==
            std::vector<Domain::PathText>{root});
    legacyContinuity.setGetOutcome(
        Domain::LegacyContinuityGetOutcome{recoveredRecord, true});

    auto recovered = invoke(
        "context_get",
        Json{{"handoff_id", handoffId.value()}, {"resume_ready", true}}.dump());
    REQUIRE(recovered);
    REQUIRE(recovered.value().receipt.ok);
    REQUIRE(recovered.value().contextRecovery.has_value());
    REQUIRE(recovered.value().contextRecovery->handoffId == handoffId);
    REQUIRE(recovered.value().contextRecovery->workingDirectory ==
            std::optional<Domain::PathText>{recoveredWorkingDirectory});
    REQUIRE(recovered.value().contextRecovery->keyFiles ==
            std::vector<Domain::PathText>{recoveredKeyFile});
    REQUIRE(!recovered.value().continuityObservation);
    const auto recoveredPayload = Json::parse(
        recovered.value().canonicalPayload);
    REQUIRE(recoveredPayload.at("found") == true);
    REQUIRE(recoveredPayload.at("context_budget_cleared") == true);
    REQUIRE(clientWorkspaceContext.adoptCalls() == 1U);

    auto resumedForgeStatus = invoke("forge_status", "{}");
    REQUIRE(resumedForgeStatus);
    const auto resumedPayload = Json::parse(
        resumedForgeStatus.value().canonicalPayload);
    const auto& resumedAutomatic = resumedPayload.at("auto_continuity");
    REQUIRE(resumedAutomatic.at("progress_count") == 0U);
    REQUIRE(resumedAutomatic.at("blocked") == false);
    REQUIRE(resumedAutomatic.at("handoff_id") == handoffId.value());
    REQUIRE(resumedAutomatic.at("implicit_roots") == Json::array(
        {root.value(),
         recoveredWorkingDirectory.value(),
         recoveredKeyFileRoot.value()}));

    const auto finalSnapshot = guard->snapshot(clientId);
    REQUIRE(!finalSnapshot.blocked);
    REQUIRE(finalSnapshot.handoffId ==
            std::optional<std::string>{handoffId.value()});
    REQUIRE((finalSnapshot.implicitRoots ==
             std::vector<Domain::PathText>{
                 root, recoveredWorkingDirectory, recoveredKeyFileRoot}));
    REQUIRE(audit.eventCount() == 5U);
}

} // namespace

int main()
{
    try {
        testHandlerContract();
        testAllCatalogPacksAreBoundedByTheAdapterContract();
        testRuntimeDispatchAndSchemaPolicy();
        testRealRouterContinuityIntegration();
        std::cout << "MCP tool-pack adapter tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP tool-pack adapter tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
