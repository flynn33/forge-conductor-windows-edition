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
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
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

class ProjectModeWorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    ProjectModeWorkspaceAuthority(Domain::AuthorityId id, Domain::ClientId caller, std::vector<Domain::PathText> roots,
        Domain::FileAccess intent, std::vector<Domain::FileAccess> grants, std::vector<Domain::FileAccess> denials,
        bool shell, std::uint64_t generation)
        : workspace_{std::move(id), std::move(caller), std::move(roots), intent, std::move(grants), std::move(denials), shell, generation}
    {}
    void enableHost(const Contracts::WorkspaceAuthority& baseline, Domain::PathText project) {
        project_ = std::move(project);
        host_ = std::make_unique<Fakes::DeterministicWorkspaceAuthority>(baseline.authorityId(), baseline.callerId(),
            std::vector<Domain::PathText>{take(Domain::PathText::create("D:/")), take(Domain::PathText::create("E:/"))},
            baseline.intent(), baseline.grants(), baseline.denials(), baseline.shellEnabled(), baseline.generation());
    }
    void restoreWorkspace() { host_.reset(); project_.reset(); }
    Domain::FileSystemAccessMode fileSystemAccessMode() const noexcept override
    { return host_ ? Domain::FileSystemAccessMode::Host : Domain::FileSystemAccessMode::Workspace; }
    Domain::Result<Domain::PathText> defaultWorkspacePath(const Contracts::WorkspaceAuthority& token,
        const Domain::OperationContext& context) noexcept override {
        return project_ ? Domain::Result<Domain::PathText>::success(*project_) :
            Contracts::IWorkspaceAuthority::defaultWorkspacePath(token, context);
    }
    Domain::Result<Contracts::WorkspaceAuthority> authorityFor(const Domain::ProjectId& id,
        const Domain::OperationContext& context) noexcept override { return delegate().authorityFor(id, context); }
    Domain::Result<Contracts::WorkspaceAuthority> narrow(const Contracts::WorkspaceAuthority& token,
        const std::vector<Domain::PathText>& roots, const std::vector<Domain::FileAccess>& grants, bool shell,
        std::uint64_t generation, const Domain::OperationContext& context) noexcept override
    { return delegate().narrow(token, roots, grants, shell, generation, context); }
    Domain::Result<Contracts::AuthorizedPath> authorize(const Contracts::WorkspaceAuthority& token,
        const Domain::PathAuthorizationRequest& request, const Domain::OperationContext& context) noexcept override {
        lastExcludedSubtree = request.excludedSubtree;
        if (request.excludedSubtree) {
            const auto& excluded = request.excludedSubtree->value();
            const auto& path = request.requestedPath.value();
            if (path == excluded || path.starts_with(excluded + "/") || path.starts_with(excluded + "\\"))
                return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,
                    "The fake path is inside the excluded project subtree."));
        }
        return delegate().authorize(token, request, context);
    }
    std::optional<Domain::PathText> lastExcludedSubtree;
private:
    Fakes::DeterministicWorkspaceAuthority& delegate() { return host_ ? *host_ : workspace_; }
    Fakes::DeterministicWorkspaceAuthority workspace_;
    std::unique_ptr<Fakes::DeterministicWorkspaceAuthority> host_;
    std::optional<Domain::PathText> project_;
};

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

class StaticProjectPolicy final : public Contracts::IProjectPolicyService {
public:
    explicit StaticProjectPolicy(std::string inspection)
        : inspection_{std::move(inspection)}
    {
    }

    [[nodiscard]] Domain::Result<std::string> execute(
        const Contracts::ProjectPolicyRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        if (captureEvidence && request.action == Contracts::ProjectPolicyAction::Evaluate) {
            evidence.push_back(request.detailsJson);
            return Domain::Result<std::string>::success("{}");
        }
        if (request.action != Contracts::ProjectPolicyAction::Inspect) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The static policy fake only supports inspection."));
        }
        return Domain::Result<std::string>::success(inspection_);
    }

    [[nodiscard]] Domain::Result<void> check(
        const Domain::ToolAuthorizationRequest&,
        const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    void setInspection(std::string inspection) { inspection_ = std::move(inspection); }
    bool captureEvidence{};
    std::vector<std::string> evidence;

private:
    std::string inspection_;
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

    void setPersistOutcome(Domain::LegacyContinuityPersistOutcome outcome)
    {
        persistOutcome_ = std::move(outcome);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        if (persistOutcome_) {
            return Domain::Result<Domain::LegacyContinuityPersistOutcome>::success(
                *persistOutcome_);
        }
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        ++handoffCalls_;
        lastHandoffRequest_ = request;
        if (persistOutcome_) {
            return Domain::Result<Domain::LegacyContinuityPersistOutcome>::success(
                *persistOutcome_);
        }
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
        const Domain::ClientId& clientId,
        const std::string_view reason,
        const Domain::OperationContext&,
        const Domain::LegacyContinuityPatch&,
        std::optional<Domain::LegacyHandoffId>) noexcept override
    {
        ++budgetCalls_;
        lastBudgetClientId_ = clientId;
        lastBudgetReason_ = reason;
        if (automaticOutcome_) {
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                *automaticOutcome_);
        }
        return unavailable<Domain::LegacyContinuityPersistOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        lastGetRequest_ = request;
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

    [[nodiscard]] const std::optional<Domain::LegacyContinuityGetRequest>& lastGetRequest() const noexcept
    {
        return lastGetRequest_;
    }

    [[nodiscard]] std::size_t handoffCalls() const noexcept
    {
        return handoffCalls_;
    }

    [[nodiscard]] const std::optional<Domain::LegacyContinuityWriteRequest>&
    lastHandoffRequest() const noexcept
    {
        return lastHandoffRequest_;
    }

    [[nodiscard]] std::size_t automaticCalls() const noexcept
    {
        return automaticCalls_;
    }

    [[nodiscard]] std::size_t budgetCalls() const noexcept
    {
        return budgetCalls_;
    }

    [[nodiscard]] const std::optional<std::string>& lastBudgetReason() const noexcept
    {
        return lastBudgetReason_;
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
    std::optional<Domain::LegacyContinuityGetRequest> lastGetRequest_;
    std::optional<Domain::LegacyContinuityStatusSummary> statusSummary_;
    std::optional<Domain::LegacyContinuityPersistOutcome> automaticOutcome_;
    std::size_t handoffCalls_{};
    std::optional<Domain::LegacyContinuityWriteRequest> lastHandoffRequest_;
    std::optional<Domain::LegacyContinuityPersistOutcome> persistOutcome_;
    std::optional<Domain::LegacyContinuityAutomaticRequest>
        lastAutomaticRequest_;
    std::optional<Domain::ClientId> lastAutomaticClientId_;
    std::size_t automaticCalls_{};
    std::optional<Domain::ClientId> lastBudgetClientId_;
    std::optional<std::string> lastBudgetReason_;
    std::size_t budgetCalls_{};
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

class NativeCapabilityRecorder final : public Contracts::IWebAccessService,
    public Contracts::IArtifactDocumentService, public Contracts::IDesktopArtifactService {
public:
    std::vector<std::string> calls;
    Json lastArguments;
    std::vector<Domain::PathText> lastRoots;
    std::optional<Json> response;
    std::optional<Domain::Error> failure;
    Domain::Result<std::string> execute(std::string_view name, std::string_view arguments,
        const Domain::OperationContext&) noexcept override
    { return record(name, arguments); }
    Domain::Result<std::string> execute(std::string_view name, std::string_view arguments,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext&) noexcept override
    { lastRoots = authority.trustedRoots(); return record(name, arguments); }
private:
    Domain::Result<std::string> record(std::string_view name, std::string_view arguments) noexcept
    {
        try { calls.emplace_back(name); lastArguments = Json::parse(arguments);
            if (failure) return Domain::Result<std::string>::failure(*failure);
            if (response) return Domain::Result<std::string>::success(response->dump());
            return Domain::Result<std::string>::success(Json{{"ok", true}, {"observed_tool", name}}.dump()); }
        catch (...) { return unavailable<std::string>("Capability test recorder failed"); }
    }
};

class StaticReviewerRuns final : public Contracts::IManagedRunService {
public:
    explicit StaticReviewerRuns(Domain::ManagedRunRecord initial) : record{std::move(initial)} {}
    Domain::ManagedRunRecord record;
    std::optional<Domain::ManagedRunStartRequest> lastStart;
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& request, const Domain::OperationContext&) noexcept override
    { lastStart = request; record.allowTools = request.allowTools;
      record.providerReceiveTimeoutSeconds = request.providerReceiveTimeoutSeconds; return snapshot(); }
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId&, const Domain::OperationContext&) noexcept override
    { return snapshot(); }
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId&, const Domain::OperationContext&) noexcept override
    { record.state = Domain::ManagedRunState::Cancelling; return snapshot(); }
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId&, const Domain::OperationContext&) noexcept override
    { return unavailable<Domain::ManagedRunSnapshot>("Unused reviewer pause"); }
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId&, const Domain::OperationContext&) noexcept override
    { return unavailable<Domain::ManagedRunSnapshot>("Unused reviewer resume"); }
    void shutdown() noexcept override {}
private:
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> snapshot() noexcept
    {
        try { return Domain::Result<Domain::ManagedRunSnapshot>::success({record, true, false, false}); }
        catch (...) { return unavailable<Domain::ManagedRunSnapshot>("Reviewer snapshot failed"); }
    }
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
    REQUIRE(tools.size() == 106U);

    const std::map<std::string_view, std::size_t> expectedPackCounts{
        {"AgentToolPack", 9U},
        {"CMakeTestToolPack", 2U},
        {"CluGovernanceToolPack", 4U},
        {"ContinuityLifecycleToolPack", 7U},
        {"ContinuityToolPack", 4U},
        {"DocsToolPack", 2U},
        {"FilesystemToolPack", 9U},
        {"GitToolPack", 5U},
        {"MemoryToolPack", 5U},
        {"ProjectMemoryToolPack", 10U},
        {"InstructionPackageToolPack", 1U},
        {"ProjectPolicyToolPack", 1U},
        {"SearchToolPack", 1U},
        {"ShellToolPack", 5U},
        {"EvidenceToolPack", 2U}, {"GitHubReadToolPack", 1U}, {"ProcessToolPack", 7U},
        {"HostInspectionToolPack", 3U}, {"ReviewerToolPack", 3U}, {"VerificationToolPack", 2U},
        {"WebAccessToolPack", 3U}, {"OfficeDocumentToolPack", 3U}, {"DesktopToolPack", 7U},
        {"ImageToolPack", 3U}, {"AgentWorkerToolPack", 3U}, {"ScheduledTaskToolPack", 4U}};
    std::map<std::string_view, std::size_t> actualPackCounts;
    for (const auto& descriptor : tools) {
        ++actualPackCounts[descriptor.tool.pack];
        REQUIRE(descriptor.tool.availability ==
                Domain::ToolAvailability::Available);
        const auto schema = Json::parse(descriptor.inputSchema);
        REQUIRE(schema.is_object());
        REQUIRE(schema.value("type", "") == "object");

        const bool closedPack =
            descriptor.tool.pack == "CMakeTestToolPack" ||
            descriptor.tool.pack == "InstructionPackageToolPack" ||
            descriptor.tool.pack == "ProjectPolicyToolPack" ||
            descriptor.tool.pack == "ProjectMemoryToolPack" ||
            descriptor.tool.pack == "ContinuityLifecycleToolPack" ||
            descriptor.tool.pack == "CluGovernanceToolPack" ||
            descriptor.tool.name.starts_with("shell_job_") ||
            descriptor.tool.name == "workspace_authority_bind" ||
            descriptor.tool.pack == "EvidenceToolPack" ||
            descriptor.tool.pack == "GitHubReadToolPack" ||
            descriptor.tool.pack == "ProcessToolPack" ||
            descriptor.tool.pack == "HostInspectionToolPack" ||
            descriptor.tool.pack == "ReviewerToolPack" ||
            descriptor.tool.pack == "VerificationToolPack" ||
            descriptor.tool.pack == "WebAccessToolPack" || descriptor.tool.pack == "OfficeDocumentToolPack" ||
            descriptor.tool.pack == "DesktopToolPack" || descriptor.tool.pack == "ImageToolPack" ||
            descriptor.tool.pack == "AgentWorkerToolPack" || descriptor.tool.pack == "ScheduledTaskToolPack";
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
    const auto secondaryRoot =
        take(Domain::PathText::create("E:/workspace-secondary"));
    const Domain::OperationContext context{
        operationId,
        Domain::MonotonicTimePoint{} + 5min,
        {},
        correlationId};

    ProjectModeWorkspaceAuthority workspaceAuthority{
        authorityId,
        clientId,
        {root, secondaryRoot},
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
        {root, secondaryRoot},
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
    const auto packageRevision = parse<Domain::Sha256Digest>(
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    const auto packageRecordId = parse<Domain::MemoryRecordId>(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const auto packageBody = Json{
        {"schema", "forge-instruction-package-queue-v2"},
        {"project_id", projectId.value()},
        {"queue_row_id", "queue-runtime-adapter"},
        {"package_id", "package-runtime-adapter"},
        {"package_name", "Runtime instructions"},
        {"package_path", "D:/instructions/runtime"},
        {"revision", packageRevision.value()},
        {"order", 1024U},
        {"state", "ready"}}.dump();
    Fakes::ScriptedHasher hasher{packageRevision};
    const auto packageTime = Domain::UtcTimePoint{1'700'000'000s};
    projectMemory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            projectId,
            {Domain::MemorySearchHit{
                Domain::ProjectMemoryRecord{
                    packageRecordId,
                    projectId,
                    1U,
                    "instruction_package_queue",
                    "Runtime instructions",
                    "One configured instruction package",
                    packageBody,
                    {"instruction-package-queue"},
                    1.0,
                    1.0,
                    "manager_instruction_package",
                    std::optional<std::string>{"D:/instructions/runtime"},
                    std::nullopt,
                    packageTime,
                    packageTime,
                    packageTime,
                    std::nullopt,
                    packageRevision,
                    false,
                    Domain::ProjectMemorySchemaVersion},
                1.0}},
            std::nullopt,
            false,
            packageBody.size(),
            256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    StaticProjectPolicy projectPolicy{Json{
        {"active", true},
        {"state", "enforcing"},
        {"source", "A:/development-policy"},
        {"revision", packageRevision.value()},
        {"entry_count", 7U},
        {"coverage_gap_count", 1U}}.dump()};
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
    continuityAutomation.setSnapshot(
        Domain::ContinuityAutomationStatusSnapshot{
            true,
            false,
            std::optional<std::string>{"automatic-handoff"},
            {root, secondaryRoot}, true});
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
        std::vector<Domain::Uuid>{parse<Domain::Uuid>("34343434-3434-4434-8434-343434343431"),
            parse<Domain::Uuid>("34343434-3434-4434-8434-343434343432"),
            parse<Domain::Uuid>("34343434-3434-4434-8434-343434343433")}};
    const auto shellExecutable = take(Domain::PathText::create(
        "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));

    bool throwToolObservation{};
    bool throwWorkspaceObservation{};
    std::optional<std::pair<Domain::ProjectId, Domain::PathText>> observedWorkspace;
    bool throwStatusObservation{};
    std::size_t toolObservationCalls{};
    std::size_t statusObservationCalls{};
    std::string visibleStatusPayload{"{}"};
    StaticReviewerRuns reviewerRuns{Domain::ManagedRunRecord{
        firstOpenSession, projectId, clientId, "Independent review", 11U,
        Domain::ManagedRunState::Completed, std::nullopt, 20U, 30U,
        std::nullopt, std::string(Domain::MaximumManagedRunOutputBytes, '\x01'),
        std::nullopt, {}, Domain::UtcTimePoint{}, Domain::UtcTimePoint{}}};
    reviewerRuns.record.readOnlyTools = true;
    auto adapterDependencies = Mcp::McpToolPackDependencies{
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
            hasher,
            Domain::ProjectMemoryLimits{},
            std::chrono::seconds{37},
            shellExecutable,
            "0.9.0",
            "windows-cpp",
            42U,
            &projectPolicy,
            "unspecified",
            [&] {
                ++statusObservationCalls;
                if (throwStatusObservation) {
                    throw std::runtime_error{"Optional chat status failed"};
                }
                return visibleStatusPayload;
            },
            [&](std::string_view, bool, std::string_view) {
                ++toolObservationCalls;
                if (throwToolObservation) {
                    throw std::runtime_error{"Optional chat tool observation failed"};
                }
            },
            [&](const Domain::ProjectId& project, const Domain::PathText& selectedRoot) {
                if (throwWorkspaceObservation) {
                    throw std::runtime_error{"Optional chat workspace observation failed"};
                }
                observedWorkspace = std::make_pair(project, selectedRoot);
            }};
    adapterDependencies.reviewerRuns = [&]() -> Contracts::IManagedRunService* { return &reviewerRuns; };
    NativeCapabilityRecorder nativeCapabilities;
    adapterDependencies.webAccess = &nativeCapabilities;
    adapterDependencies.artifactDocuments = &nativeCapabilities;
    adapterDependencies.desktopArtifacts = &nativeCapabilities;
    auto workerBrokerDependencies = adapterDependencies;
    auto brokeredBindingDependencies = adapterDependencies;
    auto brokeredExecutionDependencies = adapterDependencies;
    auto imageAnalysisDependencies = adapterDependencies;
    auto capabilityDependencies = adapterDependencies;
    auto adapter = take(Mcp::McpToolPackAdapter::create(std::move(adapterDependencies)));
    REQUIRE(adapter->tools().size() == 106U);

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

    {
        const auto imagePath = root.value() + "/sampled-image.png";
        const Json request{{"path", imagePath}, {"samples", Json::array({Json{{"x", 1}, {"y", 0}}})}};
        nativeCapabilities.response = Json{{"ok", true}, {"path", imagePath},
            {"decoded_frame_index", 0}, {"decoded_pixel_format", "RGBA8"}, {"decoded_width", 2}, {"decoded_height", 1},
            {"decoded_row_stride_bytes", 8}, {"decoded_rgba8_sha256", std::string(64, 'a')},
            {"preview_png_sha256", std::string(64, 'b')},
            {"pixel_samples", Json::array({Json{{"x", 1}, {"y", 0}, {"rgba", Json::array({17, 34, 51, 68})}}})}};
        const auto measured = Json::parse(take(adapter->handle(authorize("image_read", Domain::ToolEffect::Read,
            request.dump(), "image-read-pixel-measurements"), authority, context)).canonicalPayload);
        REQUIRE(nativeCapabilities.calls.back() == "image_read" && nativeCapabilities.lastArguments == request);
        REQUIRE(measured.at("decoded_rgba8_sha256") == nativeCapabilities.response->at("decoded_rgba8_sha256"));
        REQUIRE(measured.at("preview_png_sha256") == nativeCapabilities.response->at("preview_png_sha256"));
        REQUIRE(measured.at("decoded_pixel_format") == "RGBA8" && measured.at("decoded_frame_index") == 0);
        REQUIRE(measured.at("pixel_samples") == nativeCapabilities.response->at("pixel_samples"));
    }
    {
        const auto imagePath = root.value() + "/image-\xCE\xA9.png";
        const Json imageRequest{{"path", imagePath}, {"authorization", "Owner requested visual inspection"},
            {"question", "Describe the \"actual visible pixels\".\nReport uncertainty."}, {"receive_timeout_sec", 1800}};
        nativeCapabilities.response = Json{{"ok", true}, {"path", imagePath}, {"width", 256}, {"height", 192},
            {"format", "png"}, {"image_base64", "iVBORw0KGgo="}, {"image_mime_type", "image/png"}};
        std::size_t reviewStarts{};
        Json forwarded;
        std::optional<Domain::Error> brokerFailure;
        imageAnalysisDependencies.durableToolBroker = [&](std::string_view name, std::string_view encoded,
            const Domain::ProjectId& project, const Domain::OperationContext&) {
            REQUIRE(name == "reviewer_start" && project == projectId);
            ++reviewStarts;
            forwarded = Json::parse(encoded);
            if (brokerFailure) return Domain::Result<std::string>::failure(*brokerFailure);
            return Domain::Result<std::string>::success(Json{{"ok", true}, {"run_id", firstOpenSession.value()},
                {"state", "running"}, {"output", nullptr}, {"gate_approved", false}}.dump());
        };
        auto imageAdapter = take(Mcp::McpToolPackAdapter::create(std::move(imageAnalysisDependencies)));
        const auto analyze = [&](const Json& args, const Contracts::WorkspaceAuthority& token) {
            return imageAdapter->handle(authorizeFor("image_analyze", Domain::ToolEffect::Write,
                args.dump(), "image-analysis", token), token, context);
        };
        const auto analyzed = Json::parse(take(analyze(imageRequest, authority)).canonicalPayload);
        REQUIRE(reviewStarts == 1U && nativeCapabilities.calls.back() == "image_read");
        REQUIRE(nativeCapabilities.lastArguments == Json({{"path", imagePath}}));
        REQUIRE(forwarded.at("mode") == "tools" && forwarded.at("receive_timeout_sec") == 1800);
        REQUIRE(forwarded.at("opening_message").get<std::string>().find(Json{{"path", imagePath}}.dump()) != std::string::npos);
        REQUIRE(forwarded.at("opening_message").get<std::string>().find(imageRequest.at("question").dump()) != std::string::npos);
        REQUIRE(forwarded.at("opening_message").get<std::string>().find("not a frozen copy") != std::string::npos);
        REQUIRE(analyzed.at("state") == "running" && analyzed.at("output").is_null());
        REQUIRE(analyzed.at("run_id") == firstOpenSession.value() && analyzed.at("gate_approved") == false);
        REQUIRE(analyzed.at("image_analysis").at("poll_tool") == "reviewer_status");
        REQUIRE(analyzed.at("image_analysis").at("executor_history_included") == false);
        REQUIRE(analyzed.at("image_analysis").at("tool_scope") == "existing_project_authorized_read_only_reviewer_catalog");
        REQUIRE(analyzed.at("image_analysis").at("width") == 256);
        REQUIRE(analyzed.at("image_base64") == "iVBORw0KGgo=");
        REQUIRE(analyzed.at("image_analysis").at("preview_max_dimension_requested") == 256);
        auto detailedRequest = imageRequest; detailedRequest["preview_max_dimension"] = 1024;
        const auto baseImageResponse = nativeCapabilities.response;
        nativeCapabilities.response->update(Json{{"width", 1024}, {"height", 768},
            {"preview_width", 768}, {"preview_height", 576}, {"preview_max_dimension_requested", 1024},
            {"preview_encoded_bytes", 12}, {"preview_encoded_byte_limit", 512U * 1024U},
            {"preview_reduced_for_byte_limit", true}});
        const auto detailed = Json::parse(take(analyze(detailedRequest, authority)).canonicalPayload);
        const Json expectedImageArguments{{"path", imagePath}, {"preview_max_dimension", 1024}};
        REQUIRE(nativeCapabilities.lastArguments == expectedImageArguments);
        REQUIRE(forwarded.at("opening_message").get<std::string>().find(expectedImageArguments.dump()) != std::string::npos);
        REQUIRE(detailed.at("state") == "running" && detailed.at("output").is_null() && detailed.at("gate_approved") == false);
        REQUIRE(detailed.at("image_analysis").at("preview_max_dimension_requested") == 1024);
        REQUIRE(detailed.at("preview_max_dimension_requested") == 1024 && detailed.at("preview_width") == 768 &&
            detailed.at("preview_height") == 576 && detailed.at("preview_reduced_for_byte_limit") == true &&
            detailed.at("preview_encoded_byte_limit") == 512U * 1024U && detailed.at("preview_encoded_bytes") == 12);
        nativeCapabilities.response = baseImageResponse;
        auto defaults = imageRequest; defaults.erase("question"); defaults.erase("receive_timeout_sec");
        REQUIRE(analyze(defaults, authority));
        REQUIRE(forwarded.at("receive_timeout_sec") == 600);
        REQUIRE(forwarded.at("opening_message").get<std::string>().find("Describe the visible background") != std::string::npos);
        const auto startsBeforeInvalid = reviewStarts;
        const auto readsBeforeInvalid = nativeCapabilities.calls.size();
        for (const auto& invalid : std::vector<Json>{
            Json{{"path", imagePath}}, Json{{"path", imagePath}, {"authorization", ""}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"question", ""}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"question", std::string(4097U, 'x')}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"question", std::string(4095U, 'x') + "\xCE\xA9"}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"question", std::string("NUL\0text", 8)}},
            Json{{"path", imagePath}, {"authorization", std::string(1025U, 'a')}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"receive_timeout_sec", 0}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"receive_timeout_sec", 3601}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"preview_max_dimension", 127}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"preview_max_dimension", 2049}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"preview_max_dimension", "1024"}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"preview_max_dimension", 512.5}},
            Json{{"path", imagePath}, {"authorization", "owner"}, {"unknown", true}}}) REQUIRE(!analyze(invalid, authority));
        auto outside = imageRequest; outside["path"] = "Z:/outside/image.png";
        const auto deniedPath = analyze(outside, authority);
        REQUIRE(!deniedPath && deniedPath.error().code == Domain::ErrorCodes::Unauthorized);
        auto readOnly = take(workspaceAuthority.narrow(authority, {root}, {Domain::FileAccess::Read}, false, 12U, context));
        const auto deniedReadOnly = analyze(imageRequest, readOnly);
        REQUIRE(!deniedReadOnly && deniedReadOnly.error().code == Domain::ErrorCodes::Unauthorized);
        REQUIRE(nativeCapabilities.calls.size() == readsBeforeInvalid && reviewStarts == startsBeforeInvalid);
        auto encodedLimit = imageRequest;
        encodedLimit["path"] = root.value() + "/" + std::string(32'768U - root.value().size() - 1U, '\x01');
        const auto oversizedOpening = analyze(encodedLimit, authority);
        REQUIRE(!oversizedOpening && oversizedOpening.error().code == Domain::ErrorCodes::PayloadTooLarge);
        REQUIRE(reviewStarts == startsBeforeInvalid);
        nativeCapabilities.failure = Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Native codec rejected invalid image bytes");
        const auto badImage = analyze(imageRequest, authority);
        REQUIRE(!badImage && badImage.error().message == "Native codec rejected invalid image bytes");
        REQUIRE(reviewStarts == startsBeforeInvalid);
        nativeCapabilities.failure.reset();
        for (const auto& malformed : std::vector<Json>{
                Json{{"ok", true}},
                Json{{"ok", true}, {"width", 0}, {"height", 192}, {"format", "png"}},
                Json{{"ok", true}, {"width", 256}, {"height", 4097}, {"format", "png"}},
                Json{{"ok", true}, {"width", "256"}, {"height", 192}, {"format", "png"}}}) {
            nativeCapabilities.response = malformed;
            const auto rejected = analyze(imageRequest, authority);
            REQUIRE(!rejected && rejected.error().code == Domain::ErrorCodes::InternalFailure);
            REQUIRE(reviewStarts == startsBeforeInvalid);
        }
        nativeCapabilities.response = Json{{"ok", true}, {"width", 256}, {"height", 192}, {"format", "png"}};
        brokerFailure = Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, "Controlled unavailable Manager", true);
        const auto failedBroker = analyze(imageRequest, authority);
        REQUIRE(!failedBroker && failedBroker.error().code == Domain::ErrorCodes::HostCapabilityUnavailable);
        REQUIRE(failedBroker.error().retryable);
        nativeCapabilities.response.reset();
    }

    {
        const auto capabilities = Json::parse(take(adapter->handle(authorize("host_capabilities", Domain::ToolEffect::Read,
            "{}", "native-capability-report"), authority, context)).canonicalPayload);
        REQUIRE(capabilities.at("tool_count") == 106U);
        REQUIRE(capabilities.at("dedicated").at("word_excel_powerpoint_creation") == true);
        REQUIRE(capabilities.at("dedicated").at("independent_image_analysis") == true);
        auto noReviewDependencies = capabilityDependencies;
        noReviewDependencies.reviewerRuns = []() -> Contracts::IManagedRunService* { return nullptr; };
        auto noReview = take(Mcp::McpToolPackAdapter::create(std::move(noReviewDependencies)));
        const auto unavailable = Json::parse(take(noReview->handle(authorize("host_capabilities", Domain::ToolEffect::Read,
            "{}", "no-image-review-service"), authority, context)).canonicalPayload);
        REQUIRE(unavailable.at("dedicated").at("independent_image_analysis") == false);
        capabilityDependencies.desktopArtifacts = nullptr;
        auto noDecoder = take(Mcp::McpToolPackAdapter::create(std::move(capabilityDependencies)));
        const auto noDecodeCapabilities = Json::parse(take(noDecoder->handle(authorize("host_capabilities", Domain::ToolEffect::Read,
            "{}", "no-image-decoder"), authority, context)).canonicalPayload);
        REQUIRE(noDecodeCapabilities.at("dedicated").at("independent_image_analysis") == false);
        REQUIRE(capabilities.at("filesystem_access") == "workspace");
        for (const auto& [name, arguments] : std::vector<std::pair<std::string, Json>>{
            {"web_fetch", {{"url", "https://example.com"}}},
            {"web_search", {{"query", "Windows"}}},
            {"http_request", {{"url", "https://example.com"}, {"method", "POST"}, {"body", "bounded"}}},
            {"document_write", {{"path", "file.docx"}, {"paragraphs", Json::array({"Unicode: \xE2\x9C\x93"})}}},
            {"spreadsheet_write", {{"path", "file.xlsx"}, {"sheets", Json::array({Json{{"name", "Sheet"}, {"rows", Json::array({Json::array({1, "literal"})})}}})}}},
            {"presentation_write", {{"path", "file.pptx"}, {"slides", Json::array({Json{{"title", "Title"}, {"body", Json::array({"Body"})}}})}}},
            {"desktop_read", {{"window_id", 42}, {"pid", 123}}},
            {"image_write", {{"path", "file.png"}, {"width", 40}, {"height", 40}, {"elements", Json::array()}}}}) {
            const auto found = std::find_if(catalog->tools().begin(), catalog->tools().end(),
                [&](const auto& descriptor) { return descriptor.tool.name == name; });
            REQUIRE(found != catalog->tools().end());
            const auto effect = found->tool.effect;
            auto outcome = take(adapter->handle(authorize(name, effect, arguments.dump(), "native-dispatch-" + name), authority, context));
            REQUIRE(outcome.receipt.ok);
            REQUIRE(nativeCapabilities.calls.back() == name);
            REQUIRE(nativeCapabilities.lastArguments == arguments);
        }
        REQUIRE(nativeCapabilities.lastRoots == authority.trustedRoots());
        const auto count = nativeCapabilities.calls.size();
        auto invalid = adapter->handle(authorize("web_fetch", Domain::ToolEffect::Read,
            R"({"url":"https://example.com","ambient_credentials":true})", "native-invalid-schema"), authority, context);
        REQUIRE(!invalid && invalid.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(nativeCapabilities.calls.size() == count);
        std::size_t workerBrokerCalls{};
        Json actualScope;
        workerBrokerDependencies.durableToolBroker = [&](std::string_view name, std::string_view arguments,
            const Domain::ProjectId&, const Domain::OperationContext&) {
            REQUIRE(name == "agent_spawn"); ++workerBrokerCalls;
            actualScope = Json::parse(arguments).at("_forge_worker_scope");
            return Domain::Result<std::string>::success(R"({"ok":true,"state":"running"})");
        };
        auto workerAdapter = take(Mcp::McpToolPackAdapter::create(std::move(workerBrokerDependencies)));
        const auto spawn = Json{{"task", "Authorized bounded worker task"}, {"authorization", "owner task"}};
        REQUIRE(take(workerAdapter->handle(authorize("agent_spawn", Domain::ToolEffect::Write,
            spawn.dump(), "worker-inherited-scope"), authority, context)).receipt.ok);
        REQUIRE(actualScope.at("trusted_roots") == Json::array({root.value(), secondaryRoot.value()}));
        REQUIRE(actualScope.at("shell_enabled") == true);
        auto forged = spawn; forged["_forge_worker_scope"] = {{"trusted_roots", Json::array({"C:/"})}};
        REQUIRE(!workerAdapter->handle(authorize("agent_spawn", Domain::ToolEffect::Write,
            forged.dump(), "worker-public-forgery"), authority, context));
        REQUIRE(workerBrokerCalls == 1U);
    }

    {
        std::size_t bindBrokerCalls{};
        Json bindBrokerPayload{{"ok", true}};
        std::optional<Domain::Error> bindBrokerError;
        brokeredBindingDependencies.durableToolBroker = [&](const std::string_view name,
            const std::string_view arguments, const Domain::ProjectId& project,
            const Domain::OperationContext&) -> Domain::Result<std::string> {
            ++bindBrokerCalls;
            REQUIRE(name == "workspace_authority_bind");
            REQUIRE(project == projectId);
            REQUIRE(Json::parse(arguments).at("root") == secondaryRoot.value());
            if (bindBrokerError) return Domain::Result<std::string>::failure(*bindBrokerError);
            return Domain::Result<std::string>::success(bindBrokerPayload.dump());
        };
        auto brokeredBindingAdapter = take(Mcp::McpToolPackAdapter::create(
            std::move(brokeredBindingDependencies)));
        const auto bindCall = authorize("workspace_authority_bind", Domain::ToolEffect::Write,
            Json{{"root", secondaryRoot.value()}}.dump(), "brokered-bind-local-denied");
        const auto partial = brokeredBindingAdapter->handle(bindCall, authority, context);
        REQUIRE(!partial);
        REQUIRE(bindBrokerCalls == 1U);
        REQUIRE(partial.error().code == Domain::ErrorCodes::HostCapabilityUnavailable);
        REQUIRE(partial.error().message.find(secondaryRoot.value()) != std::string::npos);
        REQUIRE(partial.error().message.find("manager_bound=true; local_bound=false") != std::string::npos);
        REQUIRE(partial.error().message.find(
            "This host does not support binding configured workspace roots.") != std::string::npos);
        REQUIRE(partial.error().message.find("local host's root-binding capability") != std::string::npos);
        REQUIRE(partial.error().message.find("Manager binding remains active") != std::string::npos);
        REQUIRE(!partial.error().retryable);
        REQUIRE(!partial.error().evidenceId);

        const auto malformed = brokeredBindingAdapter->handle(
            authorize("workspace_authority_bind", Domain::ToolEffect::Write,
                Json{{"root", std::string(1U, '\0')}}.dump(), "bind-malformed-path"),
            authority, context);
        REQUIRE(!malformed);
        REQUIRE(malformed.error().code == Domain::ErrorCodes::MalformedMessage);
        REQUIRE(malformed.error().message.find("embedded NUL") != std::string::npos);
        REQUIRE(bindBrokerCalls == 1U);
        const auto emptyRoot = brokeredBindingAdapter->handle(
            authorize("workspace_authority_bind", Domain::ToolEffect::Write,
                Json{{"root", ""}}.dump(), "bind-empty-path"), authority, context);
        REQUIRE(!emptyRoot);
        REQUIRE(emptyRoot.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(bindBrokerCalls == 1U);

        bindBrokerPayload = Json{{"ok", "true"}};
        const auto unconfirmed = brokeredBindingAdapter->handle(bindCall, authority, context);
        REQUIRE(!unconfirmed);
        REQUIRE(unconfirmed.error().code == Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(unconfirmed.error().message.find("manager_bound=true") == std::string::npos);
        REQUIRE(bindBrokerCalls == 2U);

        bindBrokerError = Domain::makeError(Domain::ErrorCodes::Unauthorized,
            "The owner did not configure this root.", false, "manager-denial-evidence");
        const auto managerDenied = brokeredBindingAdapter->handle(bindCall, authority, context);
        REQUIRE(!managerDenied);
        REQUIRE(managerDenied.error() == *bindBrokerError);
        REQUIRE(bindBrokerCalls == 3U);

        const auto localOnly = adapter->handle(bindCall, authority, context);
        REQUIRE(!localOnly);
        REQUIRE(localOnly.error().code == Domain::ErrorCodes::HostCapabilityUnavailable);
        REQUIRE(localOnly.error().message ==
            "This host does not support binding configured workspace roots.");
    }

    {
        std::size_t executionBrokerCalls{};
        Json brokeredArguments;
        brokeredExecutionDependencies.durableToolBroker = [&](const std::string_view name,
            const std::string_view arguments, const Domain::ProjectId& project,
            const Domain::OperationContext&) -> Domain::Result<std::string> {
            REQUIRE(name == "process_launch" || name == "shell_job_start" || name == "reviewer_start" || name == "cmake_test_run" || name == "cmake_test_status");
            REQUIRE(project == projectId);
            ++executionBrokerCalls;
            brokeredArguments = Json::parse(arguments);
            return Domain::Result<std::string>::success(Json{{"ok", true}, {"job_id", "broker-job"}}.dump());
        };
        auto executionAdapter = take(Mcp::McpToolPackAdapter::create(std::move(brokeredExecutionDependencies)));
        const auto narrowed = take(shellAuthorityIssuer.narrow(shellAuthority, {root},
            shellAuthority.grants(), true, shellAuthority.generation() + 1U, context));
        for (const auto name : {"process_launch", "shell_job_start", "shell_exec"}) {
            const auto arguments = Json{{"command", "test"}, {"cwd", secondaryRoot.value()}}.dump();
            const auto denied = executionAdapter->handle(authorizeFor(name, Domain::ToolEffect::Write,
                arguments, std::string{"restricted-cwd-"} + name, narrowed), narrowed, context);
            REQUIRE(!denied);
            REQUIRE(denied.error().code == Domain::ErrorCodes::Unauthorized);
            REQUIRE(executionBrokerCalls == 0U);
        }
        for (const auto name : {"process_launch", "shell_job_start"}) {
            const auto arguments = Json{{"command", "test"}, {"cwd", secondaryRoot.value()}}.dump();
            const auto accepted = take(executionAdapter->handle(authorizeFor(name, Domain::ToolEffect::Write,
                arguments, std::string{"active-cwd-"} + name, shellAuthority), shellAuthority, context));
            REQUIRE(accepted.receipt.ok);
            REQUIRE(brokeredArguments.at("cwd") == secondaryRoot.value());
            REQUIRE(accepted.continuityObservation->workingDirectory == secondaryRoot);
        }
        REQUIRE(executionBrokerCalls == 2U);
        const auto implicit = take(executionAdapter->handle(authorizeFor("process_launch", Domain::ToolEffect::Write,
            R"({"command":"test"})", "broker-selected-default-cwd", narrowed), narrowed, context));
        REQUIRE(implicit.receipt.ok);
        REQUIRE(brokeredArguments.at("cwd") == root.value());
        REQUIRE(executionBrokerCalls == 3U);

        const auto reviewerOpening = [&](const std::string& path, const auto& selectedAuthority) {
            return executionAdapter->handle(authorizeFor("reviewer_start", Domain::ToolEffect::Write,
                Json{{"opening_message_path", path}, {"authorization", "human:opening-test"}}.dump(),
                "broker-opening-path", selectedAuthority), selectedAuthority, context);
        };
        const auto unselectedOpening = reviewerOpening(secondaryRoot.value() + "/opening.txt", narrowed);
        REQUIRE(!unselectedOpening);
        REQUIRE(unselectedOpening.error().code == Domain::ErrorCodes::Unauthorized);
        REQUIRE(executionBrokerCalls == 3U);
        const auto outsideOpening = reviewerOpening("Z:/outside/opening.txt", shellAuthority);
        REQUIRE(!outsideOpening);
        REQUIRE(outsideOpening.error().code == Domain::ErrorCodes::Unauthorized);
        REQUIRE(executionBrokerCalls == 3U);
        const auto selectedOpening = take(reviewerOpening(root.value() + "/opening.txt", narrowed));
        REQUIRE(selectedOpening.receipt.ok);
        REQUIRE(brokeredArguments.at("opening_message_path") == root.value() + "/opening.txt");
        const auto boundOpening = take(reviewerOpening(secondaryRoot.value() + "/opening.txt", shellAuthority));
        REQUIRE(boundOpening.receipt.ok);
        REQUIRE(brokeredArguments.at("opening_message_path") == secondaryRoot.value() + "/opening.txt");
        REQUIRE(executionBrokerCalls == 5U);

        const auto cmakeRun = [&](Json arguments, const auto& selectedAuthority) {
            return executionAdapter->handle(authorizeFor("cmake_test_run", Domain::ToolEffect::Write,
                arguments.dump(), "broker-cmake-run", selectedAuthority), selectedAuthority, context);
        };
        const auto unselectedBuild = cmakeRun(Json{{"build_dir", secondaryRoot.value()}}, narrowed);
        REQUIRE(!unselectedBuild && unselectedBuild.error().code == Domain::ErrorCodes::Unauthorized);
        const auto outsideBuild = cmakeRun(Json{{"build_dir", "Z:/outside/build"}}, shellAuthority);
        REQUIRE(!outsideBuild && outsideBuild.error().code == Domain::ErrorCodes::Unauthorized);
        const auto targetWithoutBuild = cmakeRun(Json{{"build_dir", root.value()}, {"target", "ProductAll"}}, shellAuthority);
        REQUIRE(!targetWithoutBuild && targetWithoutBuild.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(executionBrokerCalls == 5U);
        const auto acceptedBuild = take(cmakeRun(Json{{"build_dir", secondaryRoot.value()},
            {"mode", "build_and_test"}, {"filter", "UnitTests"}}, shellAuthority));
        REQUIRE(acceptedBuild.receipt.ok);
        REQUIRE(brokeredArguments.at("build_dir") == secondaryRoot.value());
        REQUIRE(brokeredArguments.at("mode") == "build_and_test");
        REQUIRE(acceptedBuild.continuityObservation->workingDirectory == secondaryRoot);
        REQUIRE(Json::parse(acceptedBuild.canonicalPayload).at("broker") == "persistent_manager");
        REQUIRE(executionBrokerCalls == 6U);
        const auto forwardedStatus = take(executionAdapter->handle(authorize("cmake_test_status", Domain::ToolEffect::Read,
            R"({"job_id":"broker-job","failure_offset":3,"max_failures":1})", "broker-cmake-status"), authority, context));
        REQUIRE(forwardedStatus.receipt.ok);
        REQUIRE(brokeredArguments.at("job_id") == "broker-job");
        REQUIRE(brokeredArguments.at("failure_offset") == 3);
        REQUIRE(brokeredArguments.at("max_failures") == 1);
        REQUIRE(executionBrokerCalls == 7U);
    }

    // A full valid control-character report expands sixfold in JSON. Every
    // status page must fit the MCP receipt and the Manager string envelope.
    REQUIRE(Json(*reviewerRuns.record.outputText).dump().size() > 1'048'576U);
    const auto review = [&](Json arguments) {
        arguments["run_id"] = firstOpenSession.value();
        auto call = authorize("reviewer_status", Domain::ToolEffect::Read,
            arguments.dump(), "review-output-page");
        return adapter->handle(call, authority, context);
    };
    reviewerRuns.record.evidenceSeal = packageRevision;
    reviewerRuns.record.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::Verified;
    const auto sealedReview = Json::parse(take(review(Json::object())).canonicalPayload);
    REQUIRE(sealedReview.at("evidence_sha256") == packageRevision.value());
    REQUIRE(sealedReview.at("evidence_integrity") == "verified");
    for (const auto& [integrity, expected] : {
            std::pair{Domain::ManagedRunEvidenceIntegrity::NotTerminal, "not_terminal"},
            std::pair{Domain::ManagedRunEvidenceIntegrity::LegacyUnsealed, "legacy_unsealed"},
            std::pair{Domain::ManagedRunEvidenceIntegrity::Mismatch, "mismatch"}}) {
        reviewerRuns.record.evidenceIntegrity = integrity;
        REQUIRE(Json::parse(take(review(Json::object())).canonicalPayload).at("evidence_integrity") == expected);
    }
    reviewerRuns.record.evidenceSeal.reset();
    reviewerRuns.record.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::LegacyUnsealed;
    REQUIRE(Json::parse(take(review(Json::object())).canonicalPayload).at("evidence_sha256").is_null());
    std::string reconstructed;
    std::size_t outputOffset{};
    do {
        auto outcome = take(review(Json{{"output_offset", outputOffset}}));
        REQUIRE(outcome.canonicalPayload.size() < 256U * 1024U);
        REQUIRE((Json{{"canonical_payload", outcome.canonicalPayload}}.dump().size() < 512U * 1024U));
        const auto page = Json::parse(outcome.canonicalPayload);
        REQUIRE(page.at("output_offset") == outputOffset);
        REQUIRE(page.at("output_bytes_returned") == 32U * 1024U);
        REQUIRE(page.at("output_total_bytes") == Domain::MaximumManagedRunOutputBytes);
        REQUIRE(page.at("output_page_truncated").is_boolean());
        REQUIRE(page.at("output_truncated") == false);
        REQUIRE(page.at("read_only") == true);
        REQUIRE(page.at("authorization_reference_is_human_proof") == false);
        REQUIRE(page.at("gate_approved") == false);
        reconstructed += page.at("output").get<std::string>();
        if (!page.at("output_has_more").get<bool>()) {
            REQUIRE(page.at("next_output_offset").is_null());
            break;
        }
        outputOffset = page.at("next_output_offset").get<std::size_t>();
    } while (outputOffset < Domain::MaximumManagedRunOutputBytes);
    REQUIRE(reconstructed == *reviewerRuns.record.outputText);
    REQUIRE(reviewerRuns.record.outputText->size() == Domain::MaximumManagedRunOutputBytes);

    reviewerRuns.record.outputText = std::string("AA\xe2\x82\xac") + "B";
    const auto splitPage = Json::parse(take(review(Json{{"max_output_bytes", 4}})).canonicalPayload);
    REQUIRE(splitPage.at("output") == "AA");
    REQUIRE(splitPage.at("next_output_offset") == 2);
    const auto splitLast = Json::parse(take(review(Json{{"output_offset", 2}, {"max_output_bytes", 4}})).canonicalPayload);
    REQUIRE(splitLast.at("output") == std::string("\xe2\x82\xac") + "B");
    REQUIRE(splitLast.at("output_has_more") == false);
    reviewerRuns.record.outputText = std::string("A\xe2\x82\xac") + "B";
    reviewerRuns.record.outputTruncated = true;
    const auto utf8Page = Json::parse(take(review(Json{{"max_output_bytes", 4}})).canonicalPayload);
    REQUIRE(utf8Page.at("output") == std::string("A\xe2\x82\xac"));
    REQUIRE(utf8Page.at("next_output_offset") == 4);
    REQUIRE(utf8Page.at("output_truncated") == true);
    const auto utf8Last = Json::parse(take(review(Json{{"output_offset", 4}})).canonicalPayload);
    REQUIRE(utf8Last.at("output") == "B");
    REQUIRE(utf8Last.at("output_has_more") == false);
    REQUIRE(utf8Last.at("output_page_truncated") == true);
    auto invalidReview = review(Json{{"output_offset", 2}});
    REQUIRE(!invalidReview);
    REQUIRE(invalidReview.error().code == Domain::ErrorCodes::InvalidRequest);
    invalidReview = review(Json{{"output_offset", 6}});
    REQUIRE(!invalidReview);
    invalidReview = review(Json{{"max_output_bytes", 32769}});
    REQUIRE(!invalidReview);
    invalidReview = review(Json{{"max_output_bytes", 0}});
    REQUIRE(!invalidReview);
    invalidReview = review(Json{{"output_offset", -1}});
    REQUIRE(!invalidReview);
    const auto emptyLast = Json::parse(take(review(Json{{"output_offset", 5}})).canonicalPayload);
    REQUIRE(emptyLast.at("output") == "");
    REQUIRE(emptyLast.at("next_output_offset").is_null());
    auto cancelReview = authorize("reviewer_cancel", Domain::ToolEffect::Write,
        Json{{"run_id", firstOpenSession.value()}}.dump(), "review-cancel");
    REQUIRE(Json::parse(take(adapter->handle(cancelReview, authority, context)).canonicalPayload).at("state") == "cancelling");
    reviewerRuns.record.lastError = Domain::makeError(Domain::ErrorCodes::InternalFailure,
        std::string(256U * 1024U, '\x01'));
    const auto errorOutcome = take(review(Json::object()));
    const auto errorPage = Json::parse(errorOutcome.canonicalPayload).at("error");
    REQUIRE(errorPage.at("message").get<std::string>().size() == 4U * 1024U);
    REQUIRE(errorPage.at("message_total_bytes") == 256U * 1024U);
    REQUIRE(errorPage.at("message_truncated") == true);
    REQUIRE(errorOutcome.canonicalPayload.size() < 256U * 1024U);
    REQUIRE(reviewerRuns.record.lastError->message.size() == 256U * 1024U);
    reviewerRuns.record.lastError.reset();
    reviewerRuns.record.outputText.reset();
    const auto noOutput = Json::parse(take(review(Json::object())).canonicalPayload);
    REQUIRE(noOutput.at("output").is_null());
    REQUIRE(noOutput.at("output_total_bytes") == 0);

    const auto startReview = [&](const Json& args) {
        return adapter->handle(authorize("reviewer_start", Domain::ToolEffect::Write,
            args.dump(), "reviewer-opening-test"), authority, context);
    };
    const auto callsBeforeOpening = fileSystem.calls();
    auto inlineReview = Json{{"opening_message", "Review this supplied evidence."}, {"authorization", "Owner requested"}};
    const auto inlineStarted = Json::parse(take(startReview(inlineReview)).canonicalPayload);
    REQUIRE(reviewerRuns.lastStart.has_value());
    REQUIRE(reviewerRuns.lastStart->readOnlyTools && reviewerRuns.lastStart->allowTools);
    REQUIRE(!reviewerRuns.lastStart->automaticContinuity);
    REQUIRE(reviewerRuns.lastStart->providerReceiveTimeoutSeconds == Domain::DefaultReviewerReceiveTimeoutSeconds);
    REQUIRE(reviewerRuns.lastStart->task.find("Opening message source: inline tool argument") != std::string::npos);
    REQUIRE(inlineStarted.at("receive_timeout_sec") == 600U);
    REQUIRE(fileSystem.calls() == callsBeforeOpening);
    inlineReview["opening_message"] = std::string(Domain::MaximumReviewerOpeningMessageBytes, 'a');
    inlineReview["mode"] = "text_only";
    inlineReview["receive_timeout_sec"] = 1800U;
    const auto textOnly = Json::parse(take(startReview(inlineReview)).canonicalPayload);
    REQUIRE(textOnly.at("mode") == "text_only");
    REQUIRE(textOnly.at("receive_timeout_sec") == 1800U);
    REQUIRE(!reviewerRuns.lastStart->allowTools && reviewerRuns.lastStart->readOnlyTools);
    REQUIRE(reviewerRuns.lastStart->task.find("identify any missing external evidence as unverified") != std::string::npos);
    const auto startsConsumed = uuidGenerator.consumed();
    for (const auto& invalidOpening : {
            Json{{"authorization", "Owner requested"}},
            Json{{"authorization", "Owner requested"}, {"opening_message", "inline"}, {"opening_message_path", "D:/workspace/opening.txt"}},
            Json{{"authorization", "Owner requested"}, {"opening_message", ""}},
            Json{{"authorization", "Owner requested"}, {"opening_message", std::string(Domain::MaximumReviewerOpeningMessageBytes + 1U, 'a')}},
            Json{{"authorization", "Owner requested"}, {"opening_message", std::string("invalid\0text", 12U)}},
            Json{{"authorization", "Owner requested"}, {"opening_message", "inline"}, {"mode", "invalid"}},
            Json{{"authorization", "Owner requested"}, {"opening_message", "inline"}, {"receive_timeout_sec", 0}},
            Json{{"authorization", "Owner requested"}, {"opening_message", "inline"}, {"receive_timeout_sec", 3601}},
            Json{{"authorization", "Owner requested"}, {"opening_message_path", "Z:/outside/opening.txt"}}}) {
        REQUIRE(!startReview(invalidOpening));
    }
    REQUIRE(uuidGenerator.consumed() == startsConsumed);
    REQUIRE(fileSystem.calls() == callsBeforeOpening);
    reviewerRuns.record.state = Domain::ManagedRunState::Failed;
    reviewerRuns.record.lastError = Domain::makeError(Domain::ErrorCodes::DeadlineExceeded, "Controlled provider timeout", true);
    const auto infrastructureFailure = Json::parse(take(review(Json::object())).canonicalPayload);
    REQUIRE(infrastructureFailure.at("state") == "failed");
    REQUIRE(infrastructureFailure.at("infrastructure_blocked") == true);
    REQUIRE(infrastructureFailure.at("failure_category") == "infrastructure");
    REQUIRE(infrastructureFailure.at("gate_approved") == false);
    REQUIRE(infrastructureFailure.at("error").at("retryable") == true);
    reviewerRuns.record.lastError = Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Controlled request rejection");
    REQUIRE(Json::parse(take(review(Json::object())).canonicalPayload).at("infrastructure_blocked") == false);
    reviewerRuns.record.lastError.reset();

    {
        const auto prior = reviewerRuns.record;
        reviewerRuns.record.state = Domain::ManagedRunState::Running;
        reviewerRuns.record.outputText.reset();
        nativeCapabilities.response = Json{{"ok", true}, {"width", 24}, {"height", 16}, {"format", "png"}};
        const auto request = Json{{"path", root.value() + "/local-image.png"}, {"authorization", "Owner requested image review"}};
        const auto admitted = Json::parse(take(adapter->handle(authorize("image_analyze", Domain::ToolEffect::Write,
            request.dump(), "local-image-analysis"), authority, context)).canonicalPayload);
        REQUIRE(admitted.at("state") == "running" && admitted.at("output").is_null());
        REQUIRE(reviewerRuns.lastStart->readOnlyTools && reviewerRuns.lastStart->allowTools);
        REQUIRE(!reviewerRuns.lastStart->automaticContinuity);
        REQUIRE(reviewerRuns.lastStart->providerReceiveTimeoutSeconds == 600U);
        REQUIRE(reviewerRuns.lastStart->task.find("You have no executor conversation history") != std::string::npos);
        REQUIRE(reviewerRuns.lastStart->task.find("Use image_read exactly once") != std::string::npos);
        nativeCapabilities.response.reset();
        reviewerRuns.record = prior;
    }

    auto cluFindingsCall = authorize(
        "clu.findings",
        Domain::ToolEffect::Read,
        "{}",
        "request-clu-findings");
    auto cluFindings = adapter->handle(
        cluFindingsCall, authority, context);
    REQUIRE(!cluFindings);
    REQUIRE(cluFindings.error().code == Domain::ErrorCodes::InvalidRequest);

    auto orderPage = projectMemory.listRecentResult.get().value();
    orderPage.records.clear();
    projectMemory.listRecentByKind["instruction_package_queue_order"].set(
        Domain::Result<Domain::MemoryPage>::success(orderPage));
    auto forgeStatusCall = authorize(
        "forge_status",
        Domain::ToolEffect::Read,
        "{}",
        "request-forge-status");
    auto forgeStatusResult = adapter->handle(
        forgeStatusCall, authority, context);
    REQUIRE(forgeStatusResult);
    REQUIRE(observedWorkspace.has_value());
    REQUIRE(observedWorkspace->first == projectId);
    REQUIRE(observedWorkspace->second == root);
    const auto forgeStatusPayload = Json::parse(
        forgeStatusResult.value().canonicalPayload);
    REQUIRE(forgeStatusPayload.at("ok") == true);
    REQUIRE(forgeStatusPayload.at("shell_execution").at("synchronous_timeout_sec_max") == 120);
    REQUIRE(forgeStatusPayload.at("shell_execution").at("maximum_command_bytes") == 65'536U);
    REQUIRE(forgeStatusPayload.at("shell_execution").at("jobs_available") == true);
    REQUIRE(forgeStatusPayload.at("shell_execution").at("detached_processes_survive") == false);
    REQUIRE(forgeStatusPayload.at("shell_execution").at("job_timeout_sec_max") == 3600);
    REQUIRE(forgeStatusPayload.at("workspace_authority").at("configured_roots_active_by_default") == false);
    REQUIRE(forgeStatusPayload.at("workspace_authority").at("cwd_policy") ==
        "shell_exec_shell_job_start_and_process_launch_require_locally_active_execute_authority");
    REQUIRE(forgeStatusPayload.at("home") == root.value());
    REQUIRE(forgeStatusPayload.at("presence_count") == 3U);
    REQUIRE(forgeStatusPayload.at("open_sessions") == 2U);
    REQUIRE(forgeStatusPayload.at("open_session_ids") == Json::array(
        {firstOpenSession.value(), secondOpenSession.value()}));
    REQUIRE(forgeStatusPayload.at("workspace").at("project_root") ==
            root.value());
    REQUIRE(forgeStatusPayload.at("workspace").at("project_id") ==
            projectId.value());
    REQUIRE(forgeStatusPayload.at("workspace").at("binding_source") ==
            "registered_project");
    REQUIRE(forgeStatusPayload.at("home_kind") == "application_data");
    REQUIRE(forgeStatusPayload.at("home_is_project") == false);
    REQUIRE(forgeStatusPayload.at("tool_count") == catalog->tools().size());
    REQUIRE(forgeStatusPayload.at("agent_count") == forgeStatusPayload.at("agents").size());
    auto aliasCall = authorize("get_forge_status", Domain::ToolEffect::Read, "{}", "status-alias");
    auto alias = take(adapter->handle(aliasCall, authority, context));
    REQUIRE(Json::parse(alias.canonicalPayload).at("workspace") == forgeStatusPayload.at("workspace"));
    auto entryPage = projectMemory.listRecentResult.get().value();
    auto& entryRecord = entryPage.records.front().record;
    entryRecord.kind = "instruction_package_entry";
    const std::string longText(20U * 1024U, 'x');
    entryRecord.body = Json{{"queue_row_id", "queue-runtime-adapter"},
        {"revision", packageRevision.value()}, {"relative_path", "task.txt"},
        {"derived_text", longText}, {"interpretation", "interpreted"}}.dump();
    projectMemory.searchResult.set(Domain::Result<Domain::MemoryPage>::success(entryPage));
    auto readPackage = authorize("instruction_package.read", Domain::ToolEffect::Read,
        R"({"queue_row_id":"queue-runtime-adapter"})", "package-read");
    auto firstPage = Json::parse(take(adapter->handle(readPackage, authority, context)).canonicalPayload);
    REQUIRE(firstPage.at("entries").at(0).at("content") == longText.substr(0, 16U * 1024U));
    REQUIRE(firstPage.at("entries").at(0).at("complete") == false);
    auto nextPackage = authorize("instruction_package.read", Domain::ToolEffect::Read,
        R"({"queue_row_id":"queue-runtime-adapter","path":"task.txt","offset":16384})", "package-next");
    auto lastPage = Json::parse(take(adapter->handle(nextPackage, authority, context)).canonicalPayload);
    REQUIRE(lastPage.at("entries").at(0).at("content") == longText.substr(16U * 1024U));
    REQUIRE(lastPage.at("entries").at(0).at("complete") == true);

    const auto savedInspection = Json{{"active", true}, {"state", "enforcing"},
        {"source", "A:/development-policy"}, {"revision", packageRevision.value()},
        {"entry_count", 7U}, {"coverage_gap_count", 1U}};
    const std::string multibyte = "\xe6\xb8\xac";
    auto evidenceIndex = savedInspection;
    evidenceIndex["coverage"] = Json::array();
    evidenceIndex["agent_guidance"] = Json::array();
    evidenceIndex["agent_guidance"].push_back(Json{{"evidence", ""}});
    auto encodedEvidence = evidenceIndex.dump();
    const auto contentStart = encodedEvidence.find("\"evidence\":\"\"") + std::string{"\"evidence\":\""}.size();
    REQUIRE(contentStart < encodedEvidence.size());
    evidenceIndex["agent_guidance"][0]["evidence"] =
        std::string(32U * 1024U - contentStart - 1U, 'x') + multibyte;
    projectPolicy.setInspection(evidenceIndex.dump());
    projectPolicy.captureEvidence = true;
    auto evidenceRead = authorize("project_policy.read", Domain::ToolEffect::Read, "{}", "policy-utf8-evidence");
    auto utf8EvidenceResult = adapter->handle(evidenceRead, authority, context);
    REQUIRE(!utf8EvidenceResult);
    REQUIRE(utf8EvidenceResult.error().code == Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(!projectPolicy.evidence.empty());
    const auto evidencePayload = Json::parse(projectPolicy.evidence.back());
    REQUIRE(evidencePayload.at("result").get<std::string>().size() < 32U * 1024U);
    REQUIRE(evidencePayload.at("result").get<std::string>().ends_with('x'));
    projectPolicy.captureEvidence = false;
    projectPolicy.setInspection(savedInspection.dump());
    auto smallPolicyRead = authorize("project_policy.read", Domain::ToolEffect::Read, "{}", "policy-small-index");
    const auto smallPolicy = Json::parse(take(adapter->handle(smallPolicyRead, authority, context)).canonicalPayload);
    REQUIRE(smallPolicy.at("entry_count") == 7U);
    REQUIRE(smallPolicy.at("complete") == true);
    REQUIRE(smallPolicy.at("next_cursor").is_null());
    REQUIRE(smallPolicy.at("clu_governance_notifications_deferred") == true);
    auto oversizedBase = savedInspection;
    oversizedBase["source"] = std::string(33U * 1024U, 'x');
    projectPolicy.setInspection(oversizedBase.dump());
    auto oversizedPolicy = adapter->handle(smallPolicyRead, authority, context);
    REQUIRE(!oversizedPolicy && oversizedPolicy.error().code == Domain::ErrorCodes::PayloadTooLarge);
    projectPolicy.setInspection(savedInspection.dump());
    auto oversizedEntryPage = entryPage;
    auto oversizedEntry = Json::parse(*oversizedEntryPage.records.front().record.body);
    oversizedEntry["coverage_detail"] = std::string(33U * 1024U, 'x');
    oversizedEntryPage.records.front().record.body = oversizedEntry.dump();
    projectMemory.searchResult.set(Domain::Result<Domain::MemoryPage>::success(oversizedEntryPage));
    auto oversizedPackage = adapter->handle(readPackage, authority, context);
    REQUIRE(!oversizedPackage && oversizedPackage.error().code == Domain::ErrorCodes::PayloadTooLarge);
    projectMemory.searchResult.set(Domain::Result<Domain::MemoryPage>::success(entryPage));
    REQUIRE(forgeStatusPayload.at("instruction_packages").at("count") == 1U);
    REQUIRE(forgeStatusPayload.at("instruction_packages").at("read_in_order") ==
            true);
    REQUIRE(forgeStatusPayload.at("instruction_packages").at("packages").at(0)
                .at("path") == "D:/instructions/runtime");
    REQUIRE(forgeStatusPayload.at("development_policy").at("source") ==
            "A:/development-policy");
    REQUIRE(forgeStatusPayload.at("development_policy").at("revision") ==
            packageRevision.value());
    REQUIRE(forgeStatusPayload.at("development_policy")
                .at("read_and_follow_required") == true);
    const auto bootstrap = take(adapter->bootstrapInstructions(
        projectId, root, context));
    REQUIRE(bootstrap.find("Project folder: D:/workspace") !=
            std::string::npos);
    REQUIRE(bootstrap.find("D:/instructions/runtime") != std::string::npos);
    REQUIRE(bootstrap.find("shell_job_start") != std::string::npos);
    REQUIRE(bootstrap.find("descendants are terminated") != std::string::npos);
    REQUIRE(bootstrap.find("Running is not failure") != std::string::npos);
    REQUIRE(bootstrap.find("Read and follow the instruction package folders") !=
            std::string::npos);
    REQUIRE(bootstrap.find("Development policy source: A:/development-policy") !=
            std::string::npos);
    REQUIRE(bootstrap.find("Read and follow the development policy") !=
            std::string::npos);
    const auto verifyQueueEditsReachStatusBootstrapAndReads = [&] {
        const auto statusCallsBeforeQueueEdits = forgeStatus.calls();
        auto queuePage = projectMemory.listRecentResult.get().value();
        auto firstBody = Json::parse(*queuePage.records.front().record.body);
        firstBody["state"] = "active";
        queuePage.records.front().record.body = firstBody.dump();
        auto secondRecord = queuePage.records.front().record;
        secondRecord.id = parse<Domain::MemoryRecordId>(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
        secondRecord.title = "Second instructions";
        auto secondBody = firstBody;
        secondBody["queue_row_id"] = "queue-second-adapter";
        secondBody["package_id"] = "package-second-adapter";
        secondBody["package_name"] = "Second instructions";
        secondBody["package_path"] = "D:/instructions/second";
        secondBody["order"] = 2048U;
        secondRecord.body = secondBody.dump();
        queuePage.records.push_back(Domain::MemorySearchHit{secondRecord, 1.0});
        projectMemory.listRecentByKind["instruction_package_queue"].set(
            Domain::Result<Domain::MemoryPage>::success(queuePage));

        auto currentOrderPage = orderPage;
        auto orderRecord = secondRecord;
        orderRecord.id = parse<Domain::MemoryRecordId>(
            "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
        orderRecord.kind = "instruction_package_queue_order";
        const auto publishOrder = [&](const Json& rowIds) {
            orderRecord.body = Json{
                {"schema", "forge-instruction-package-order-v1"},
                {"project_id", projectId.value()}, {"rows", rowIds}}.dump();
            currentOrderPage.records = {
                Domain::MemorySearchHit{orderRecord, 1.0}};
            projectMemory.listRecentByKind["instruction_package_queue_order"].set(
                Domain::Result<Domain::MemoryPage>::success(currentOrderPage));
        };
        const auto currentPackages = [&] {
            return Json::parse(take(adapter->handle(
                aliasCall, authority, context)).canonicalPayload)
                .at("instruction_packages");
        };
        const auto assertOrder = [&](const Json& rowIds) {
            publishOrder(rowIds);
            const auto packages = currentPackages();
            REQUIRE(packages.at("count") == rowIds.size());
            REQUIRE(packages.at("packages").at(0).at("queue_row_id") == rowIds.at(0));
            REQUIRE(packages.at("packages").at(1).at("queue_row_id") == rowIds.at(1));
            REQUIRE(packages.at("packages").at(0).at("order") == 1024U);
            REQUIRE(packages.at("packages").at(1).at("order") == 2048U);
            const auto currentBootstrap = take(adapter->bootstrapInstructions(
                projectId, root, context));
            const auto firstPath = packages.at("packages").at(0).at("path")
                .get<std::string>();
            const auto secondPath = packages.at("packages").at(1).at("path")
                .get<std::string>();
            REQUIRE(currentBootstrap.find(firstPath) != std::string::npos);
            REQUIRE(currentBootstrap.find(secondPath) != std::string::npos);
            REQUIRE(currentBootstrap.find(firstPath) < currentBootstrap.find(secondPath));
        };
        assertOrder(Json::array({"queue-second-adapter", "queue-runtime-adapter"}));
        assertOrder(Json::array({"queue-runtime-adapter", "queue-second-adapter"}));
        assertOrder(Json::array({"queue-second-adapter", "queue-runtime-adapter"}));

        queuePage.records.erase(queuePage.records.begin());
        projectMemory.listRecentByKind["instruction_package_queue"].set(
            Domain::Result<Domain::MemoryPage>::success(queuePage));
        const auto afterRemoval = currentPackages();
        REQUIRE(afterRemoval.at("count") == 1U);
        REQUIRE(afterRemoval.at("packages").at(0).at("queue_row_id") ==
                "queue-second-adapter");
        const auto afterRemovalBootstrap = take(adapter->bootstrapInstructions(
            projectId, root, context));
        REQUIRE(afterRemovalBootstrap.find("D:/instructions/runtime") ==
                std::string::npos);
        REQUIRE(afterRemovalBootstrap.find("D:/instructions/second") !=
                std::string::npos);
        const auto searchesBeforeRemovalRead = projectMemory.callCount(
            Fakes::ProjectMemoryCall::Search);
        const auto removedRead = adapter->handle(
            authorize("instruction_package.read", Domain::ToolEffect::Read,
                R"({"queue_row_id":"queue-runtime-adapter"})", "removed-package-read"),
            authority, context);
        REQUIRE(!removedRead);
        REQUIRE(removedRead.error().code == Domain::ErrorCodes::RecordNotFound);
        REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Search) ==
                searchesBeforeRemovalRead);

        queuePage.records.clear();
        projectMemory.listRecentByKind["instruction_package_queue"].set(
            Domain::Result<Domain::MemoryPage>::success(queuePage));
        publishOrder(Json::array());
        const auto emptyQueue = currentPackages();
        REQUIRE(emptyQueue.at("count") == 0U);
        REQUIRE(emptyQueue.at("packages").empty());
        const auto emptyBootstrap = take(adapter->bootstrapInstructions(
            projectId, root, context));
        REQUIRE(emptyBootstrap.find("D:/instructions/runtime") == std::string::npos);
        REQUIRE(emptyBootstrap.find("D:/instructions/second") == std::string::npos);
        REQUIRE(emptyBootstrap.find("Instruction package folders (ordered): none configured") !=
                std::string::npos);
        REQUIRE(forgeStatus.calls() == statusCallsBeforeQueueEdits + 5U);
        projectMemory.listRecentByKind.erase("instruction_package_queue");
        projectMemory.listRecentByKind["instruction_package_queue_order"].set(
            Domain::Result<Domain::MemoryPage>::success(orderPage));
    };
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
             {"note",
              "Forge checkpoints lifecycle changes and requests handoff only from measured context pressure."}}}}));
    REQUIRE((forgeStatusPayload.at("auto_continuity") == Json{
        {"resume_packet_ready", false}, {"resume_packet_id", nullptr},
        {"readback_confirmed", false}, {"readback_handoff_id", nullptr},
        {"readback_scope", "this_mcp_client_confirmed_context_get"},
        {"packet_transport", "mcp_initialize_and_context_get"},
        {"project_handoff", nullptr},
        {"enabled", true},
        {"blocked", false},
        {"handoff_pending", true},
        {"visible_chat_handoff_available", false},
        {"handoff_id", "automatic-handoff"},
        {"implicit_roots",
         Json::array({root.value(), secondaryRoot.value()})}}));
    REQUIRE(forgeStatus.calls() == 2U);

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
    REQUIRE(forgeStatus.calls() == 3U);
    forgeStatus.setProjection(Domain::ForgeStatusProjection{
        3U, {firstOpenSession, secondOpenSession}});
    verifyQueueEditsReachStatusBootstrapAndReads();

    // Optional UI diagnostics must not invalidate a completed Forge tool.
    throwToolObservation = true;
    throwWorkspaceObservation = true;
    const auto toolCallsBeforeFailure = toolObservationCalls;
    auto observedAgentListCall = authorize(
        "agent_list", Domain::ToolEffect::Read, "{}", "throwing-tool-observation");
    const auto observedAgentList = take(adapter->handle(
        observedAgentListCall, authority, context));
    REQUIRE(observedAgentList.receipt.ok);
    REQUIRE(Json::parse(observedAgentList.canonicalPayload).at("ok") == true);
    REQUIRE(Json::parse(observedAgentList.canonicalPayload).at("agents") == Json::array());
    REQUIRE(toolObservationCalls == toolCallsBeforeFailure + 1U);
    throwToolObservation = false;
    throwWorkspaceObservation = false;

    // A failed or malformed optional status leaves the core status callable.
    throwStatusObservation = true;
    const auto statusCallsBeforeFailure = statusObservationCalls;
    auto throwingStatusCall = authorize(
        "get_forge_status", Domain::ToolEffect::Read, "{}", "throwing-chat-status");
    const auto throwingStatus = take(adapter->handle(throwingStatusCall, authority, context));
    const auto throwingStatusPayload = Json::parse(throwingStatus.canonicalPayload);
    REQUIRE(throwingStatus.receipt.ok);
    REQUIRE(throwingStatusPayload.at("ok") == true);
    REQUIRE(throwingStatusPayload.at("workspace").at("project_id") == projectId.value());
    REQUIRE(throwingStatusPayload.at("tool_count") == catalog->tools().size());
    REQUIRE(throwingStatusPayload.at("visible_chat_continuity").contains("error"));
    REQUIRE(statusObservationCalls == statusCallsBeforeFailure + 1U);
    throwStatusObservation = false;

    visibleStatusPayload = "{incomplete";
    auto malformedStatusCall = authorize(
        "get_forge_status", Domain::ToolEffect::Read, "{}", "malformed-chat-status");
    const auto malformedStatus = take(adapter->handle(malformedStatusCall, authority, context));
    const auto malformedStatusPayload = Json::parse(malformedStatus.canonicalPayload);
    REQUIRE(malformedStatus.receipt.ok);
    REQUIRE(malformedStatusPayload.at("ok") == true);
    REQUIRE(malformedStatusPayload.at("workspace").at("project_id") == projectId.value());
    REQUIRE(malformedStatusPayload.at("tool_count") == catalog->tools().size());
    REQUIRE(malformedStatusPayload.at("visible_chat_continuity").is_object());
    REQUIRE(malformedStatusPayload.at("auto_continuity").at("visible_chat_handoff_available") == false);
    REQUIRE(statusObservationCalls == statusCallsBeforeFailure + 2U);
    visibleStatusPayload = "{}";

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
    // A global packet must not leak into a project's bootstrap without its pointer.
    const auto unboundBootstrap = take(adapter->bootstrapInstructions(projectId, root, context));
    REQUIRE(unboundBootstrap.find("Recovered context") == std::string::npos);
    const std::string resumePointer = "continuity/project/" + projectId.value();
    const auto beforeScopedRecovery = continuityAutomation.snapshot(clientId);
    continuityAutomation.setSnapshot({});
    auto otherProjectRecord = recoveredRecord;
    otherProjectRecord.packet.workingDirectory = "E:/unrelated/project";
    otherProjectRecord.packet.keyFiles = {"E:/unrelated/project/file.cpp"};
    legacyContinuity.setGetOutcome({otherProjectRecord, false});
    auto unrelatedCall = authorize("context_get", Domain::ToolEffect::Read, "{}", "unrelated-default-context");
    auto unrelatedResult = take(adapter->handle(unrelatedCall, authority, context));
    REQUIRE(Json::parse(unrelatedResult.canonicalPayload).at("found") == false);
    REQUIRE(!unrelatedResult.contextRecovery);
    REQUIRE(clientWorkspaceContext.adoptCalls() == 0U);
    auto pathlessRecord = recoveredRecord;
    pathlessRecord.packet.workingDirectory.reset();
    pathlessRecord.packet.keyFiles.clear();
    legacyContinuity.setGetOutcome({pathlessRecord, false});
    auto pathlessCall = authorize("context_get", Domain::ToolEffect::Read, "{}", "pathless-default-context");
    REQUIRE(Json::parse(take(adapter->handle(pathlessCall, authority, context)).canonicalPayload).at("found") == false);
    REQUIRE(clientWorkspaceContext.adoptCalls() == 0U);
    legacyContinuity.setGetOutcome({recoveredRecord, false});
    continuityAutomation.setSnapshot(beforeScopedRecovery);
    auto pendingRecord = recoveredRecord;
    pendingRecord.packet.id = parse<Domain::LegacyHandoffId>("fresh-checkpoint");
    pendingRecord.packet.resumeReady = false;
    legacyContinuity.setPersistOutcome({pendingRecord, false});
    auto initialCheckpointCall = authorize(
        "session_checkpoint", Domain::ToolEffect::Write,
        R"({"goal":"Work before handoff"})", "initial-checkpoint");
    const auto initialCheckpointResult = take(adapter->handle(initialCheckpointCall, authority, context));
    REQUIRE(initialCheckpointResult.contextPersistence == pendingRecord.packet.id);
    REQUIRE(!take(legacyMemory.get({resumePointer}, context)).note);

    const auto handoffCallsBeforeAuto = legacyContinuity.handoffCalls();
    const Json incompleteAuto{
        {"goal", "Continue the product changes"},
        {"narrative", "Summary-only completion"},
        {"resume_seed", "placeholder"}};
    for (const auto state : {"requesting_model_packet", "waiting_for_model_packet", "repairing_model_packet"}) {
        visibleStatusPayload = Json{{"state", state}}.dump();
        auto incompleteCall = authorize(
            "session_handoff", Domain::ToolEffect::Write, incompleteAuto.dump(),
            std::string{"incomplete-auto-"} + state);
        auto incomplete = adapter->handle(incompleteCall, authority, context);
        REQUIRE(!incomplete);
        REQUIRE(incomplete.error().code == Domain::ErrorCodes::InvalidRequest);
        for (const auto field : {"narrative", "resume_seed", "key_files", "next_actions", "decisions"}) {
            REQUIRE(incomplete.error().message.find(field) != std::string::npos);
        }
        REQUIRE(incomplete.error().message.find("packet_json string") != std::string::npos);
        REQUIRE(incomplete.error().message.find("same schema") != std::string::npos);
        REQUIRE(incomplete.error().message.find("Do not invent handoff_id") != std::string::npos);
        REQUIRE(incomplete.error().message.find("lorem ipsum") == std::string::npos);
        REQUIRE(incomplete.error().message.find("placeholder") == std::string::npos);
        REQUIRE(legacyContinuity.handoffCalls() == handoffCallsBeforeAuto);
        REQUIRE(!take(legacyMemory.get({resumePointer}, context)).note);
    }
    const std::string detailedNarrative =
        "Observed the adapter's session_handoff route and the project pointer publication in "
        "src/Mcp/McpToolPackAdapter.cpp. The repository is D:/workspace. The active tool catalog "
        "contains 58 tools. No install, commit, push, version bump or documentation rewrite is authorized. "
        "The successor must read this exact packet and continue the ordered actions with Forge tools available.";
    const std::string detailedSeed =
        "Resume the product task in D:/workspace. Preserve the existing three integrations and the "
        "bound package/policy order. The completed work added synchronous validation before any packet "
        "or pointer write. Verify the native successor receives the detailed packet; next inspect "
        "src/Mcp/McpToolPackAdapter.cpp, then run the narrow adapter check. No application installation, "
        "commit, push, version bump or documentation change is authorized.";
    const std::vector<std::string> explicitConstraints{
        "Do not commit, push, bump VERSION, or rewrite README, changelog, docs or wiki.",
        "Do not install over or stop the installed 1.3.4 app.",
        "Use only forge-conductor, forge-conductor-fallback and forge-conductor-clu; no fourth plugin, authentication or model-instruction files."};
    const Json completeAuto{
        {"goal", "Continue the product changes"},
        {"narrative", detailedNarrative},
        {"resume_seed", detailedSeed},
        {"decisions", explicitConstraints},
        {"key_files", Json::array({"D:/workspace/src/Mcp/McpToolPackAdapter.cpp"})},
        {"next_actions", Json::array({"Continue the source audit with fs_read and report actual findings", "Run the narrow adapter check"})}};
    auto rejectAuto = [&](Json arguments, const std::string& field, const std::string& requestId) {
        auto call = authorize("session_handoff", Domain::ToolEffect::Write, arguments.dump(), requestId);
        auto result = adapter->handle(call, authority, context);
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(result.error().message.find(field) != std::string::npos);
        REQUIRE(result.error().message.find("lorem ipsum") == std::string::npos);
        REQUIRE(result.error().message.find("placeholder") == std::string::npos);
        REQUIRE(legacyContinuity.handoffCalls() == handoffCallsBeforeAuto);
        REQUIRE(!take(legacyMemory.get({resumePointer}, context)).note);
    };
    auto missingGoal = completeAuto;
    missingGoal.erase("goal");
    rejectAuto(missingGoal, "goal", "auto-missing-goal");
    auto shortNarrative = completeAuto;
    shortNarrative["narrative"] = detailedNarrative.substr(0U, 255U);
    rejectAuto(shortNarrative, "narrative", "auto-short-narrative");
    auto shortSeed = completeAuto;
    shortSeed["resume_seed"] = detailedSeed.substr(0U, 255U);
    rejectAuto(shortSeed, "resume_seed", "auto-short-seed");
    auto placeholderSeed = completeAuto;
    placeholderSeed["resume_seed"] = detailedSeed + " placeholder";
    rejectAuto(placeholderSeed, "resume_seed", "auto-placeholder-seed");
    std::size_t fillerCase{};
    for (const auto field : {"narrative", "resume_seed"}) {
        for (const auto marker : {"Lorem ipsum dolor sit amet, consectetur adipiscing elit. ", "PLACEHOLDER draft packet. ",
                 " \tLorem ipsum dolor sit amet. ", "\nplaceholder\n"}) {
            auto fillerPacket = completeAuto;
            fillerPacket[field] = std::string{marker} + completeAuto.at(field).get<std::string>();
            rejectAuto(fillerPacket, field, "auto-filler-" + std::to_string(++fillerCase));
        }
    }
    std::size_t paddingCase{};
    for (const auto field : {"narrative", "resume_seed"}) {
        for (const auto& padding : {std::string(256U, 'a'), std::string(128U, 'a') + " \t\r\n" + std::string(128U, 'a')}) {
            auto paddedPacket = completeAuto;
            paddedPacket[field] = padding;
            rejectAuto(paddedPacket, field, "auto-padding-" + std::to_string(++paddingCase));
        }
    }
    std::size_t opaqueActionCase{};
    for (const auto action : {" continue ", "RESUME", "\tNeXt\r\n"}) {
        auto opaqueActionPacket = completeAuto;
        opaqueActionPacket["next_actions"] = Json::array({action});
        rejectAuto(opaqueActionPacket, "next_actions", "auto-opaque-action-" + std::to_string(++opaqueActionCase));
    }
    auto nestedCollections = completeAuto;
    nestedCollections["narrative"] = detailedNarrative + completeAuto.at("key_files").dump() +
        completeAuto.at("next_actions").dump();
    nestedCollections.erase("key_files");
    nestedCollections.erase("next_actions");
    rejectAuto(nestedCollections, "key_files", "auto-collections-only-in-narrative");
    auto emptyPaths = completeAuto;
    emptyPaths["key_files"] = Json::array({" "});
    rejectAuto(emptyPaths, "key_files", "auto-empty-key-path");
    auto emptyActions = completeAuto;
    emptyActions["next_actions"] = Json::array();
    rejectAuto(emptyActions, "next_actions", "auto-empty-actions");

    auto missingDecisions = completeAuto;
    missingDecisions.erase("decisions");
    rejectAuto(missingDecisions, "decisions", "auto-missing-user-constraints");
    auto emptyDecisions = completeAuto;
    emptyDecisions["decisions"] = Json::array();
    rejectAuto(emptyDecisions, "decisions", "auto-empty-user-constraints");
    auto blankDecisions = completeAuto;
    blankDecisions["decisions"] = Json::array({" \t\r\n"});
    rejectAuto(blankDecisions, "decisions", "auto-blank-user-constraints");

    auto completeAutoRecord = recoveredRecord;
    completeAutoRecord.packet.id = parse<Domain::LegacyHandoffId>("complete-auto-packet");
    completeAutoRecord.packet.narrative = detailedNarrative;
    completeAutoRecord.packet.resumeSeed = detailedSeed;
    completeAutoRecord.packet.keyFiles = {"D:/workspace/src/Mcp/McpToolPackAdapter.cpp"};
    completeAutoRecord.packet.nextActions = {"Continue the source audit with fs_read and report actual findings", "Run the narrow adapter check"};
    completeAutoRecord.packet.decisions = explicitConstraints;
    legacyContinuity.setPersistOutcome({completeAutoRecord, true});
    auto completeAutoCall = authorize(
        "session_handoff", Domain::ToolEffect::Write, completeAuto.dump(), "complete-auto-handoff");
    const auto completeAutoResult = take(adapter->handle(completeAutoCall, authority, context));
    REQUIRE(completeAutoResult.receipt.ok);
    REQUIRE(Json::parse(completeAutoResult.canonicalPayload).at("handoff_id") == completeAutoRecord.packet.id.value());
    REQUIRE(legacyContinuity.handoffCalls() == handoffCallsBeforeAuto + 1U);
    REQUIRE(legacyContinuity.lastHandoffRequest().has_value());
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.narrative == detailedNarrative);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.resumeSeed == detailedSeed);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.keyFiles == completeAutoRecord.packet.keyFiles);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.nextActions == completeAutoRecord.packet.nextActions);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.decisions == explicitConstraints);
    REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == completeAutoRecord.packet.id.value());

    const auto handoffCallsBeforeSerialized = legacyContinuity.handoffCalls();
    auto rejectSerialized = [&](Json arguments, const std::string& field, const std::string& requestId) {
        auto call = authorize("session_handoff", Domain::ToolEffect::Write, arguments.dump(), requestId);
        auto result = adapter->handle(call, authority, context);
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
        REQUIRE(result.error().message.find(field) != std::string::npos);
        REQUIRE(legacyContinuity.handoffCalls() == handoffCallsBeforeSerialized);
        REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == completeAutoRecord.packet.id.value());
    };
    rejectSerialized(Json{{"packet_json", 17U}}, "packet_json", "packet-json-not-string");
    rejectSerialized(Json{{"packet_json", "{invalid"}}, "packet_json", "packet-json-invalid");
    rejectSerialized(Json{{"packet_json", "[]"}}, "packet_json", "packet-json-array");
    rejectSerialized(Json{{"packet_json", "null"}}, "packet_json", "packet-json-null");
    rejectSerialized(Json{{"packet_json", completeAuto.dump()}, {"goal", "Conflicting outer goal"}},
        "goal", "packet-json-conflicting-goal");
    rejectSerialized(Json{{"packet_json", completeAuto.dump()}, {"key_files", Json::array({"D:/different.cpp"})}},
        "key_files", "packet-json-conflicting-collection");
    auto recursivePacket = completeAuto;
    recursivePacket["packet_json"] = completeAuto.dump();
    rejectSerialized(Json{{"packet_json", recursivePacket.dump()}}, "packet_json", "packet-json-nested-envelope");
    auto incompleteSerialized = completeAuto;
    incompleteSerialized.erase("key_files");
    incompleteSerialized.erase("next_actions");
    rejectSerialized(Json{{"packet_json", incompleteSerialized.dump()}}, "key_files", "packet-json-incomplete-auto");
    auto missingSerializedDecisions = completeAuto;
    missingSerializedDecisions.erase("decisions");
    rejectSerialized(Json{{"packet_json", missingSerializedDecisions.dump()}},
        "decisions", "packet-json-missing-user-constraints");
    auto shortSerialized = completeAuto;
    shortSerialized["resume_seed"] = detailedSeed.substr(0U, 255U);
    rejectSerialized(Json{{"packet_json", shortSerialized.dump()}}, "resume_seed", "packet-json-short-auto-seed");

    auto serializedFiller = completeAuto;
    serializedFiller["narrative"] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. " + detailedNarrative;
    rejectSerialized(Json{{"packet_json", serializedFiller.dump()}},
        "narrative", "packet-json-filler-narrative");

    auto serializedPadding = completeAuto;
    serializedPadding["resume_seed"] = std::string(128U, 'a') + " \t\r\n" + std::string(128U, 'a');
    rejectSerialized(Json{{"packet_json", serializedPadding.dump()}},
        "resume_seed", "packet-json-padding-seed");
    auto serializedOpaqueAction = completeAuto;
    serializedOpaqueAction["next_actions"] = Json::array({" Continue "});
    rejectSerialized(Json{{"packet_json", serializedOpaqueAction.dump()}},
        "next_actions", "packet-json-opaque-action");

    auto serializedRecord = completeAutoRecord;
    serializedRecord.packet.id = parse<Domain::LegacyHandoffId>("serialized-auto-packet");
    serializedRecord.packet.goal = completeAuto.at("goal").get<std::string>();
    serializedRecord.packet.status = "handoff_ready";
    serializedRecord.packet.decisions = explicitConstraints;
    legacyContinuity.setPersistOutcome({serializedRecord, true});
    auto serializedPacket = completeAuto;
    serializedPacket["handoff_id"] = serializedRecord.packet.id.value();
    serializedPacket["cwd"] = root.value();
    serializedPacket["status"] = serializedRecord.packet.status;
    serializedPacket["blockers"] = Json::array();
    serializedPacket["decisions"] = serializedRecord.packet.decisions;
    auto serializedCall = authorize("session_handoff", Domain::ToolEffect::Write,
        Json{{"packet_json", serializedPacket.dump()}, {"goal", serializedRecord.packet.goal}}.dump(),
        "packet-json-complete-auto");
    const auto serializedResult = take(adapter->handle(serializedCall, authority, context));
    REQUIRE(serializedResult.receipt.ok);
    REQUIRE(Json::parse(serializedResult.canonicalPayload).at("handoff_id") == serializedRecord.packet.id.value());
    REQUIRE(legacyContinuity.handoffCalls() == handoffCallsBeforeSerialized + 1U);
    const auto& serializedRequest = *legacyContinuity.lastHandoffRequest();
    REQUIRE(serializedRequest.handoffId == serializedRecord.packet.id);
    REQUIRE(serializedRequest.patch.goal == serializedRecord.packet.goal);
    REQUIRE(serializedRequest.patch.status == serializedRecord.packet.status);
    REQUIRE(serializedRequest.patch.workingDirectory == root.value());
    REQUIRE(serializedRequest.patch.narrative == detailedNarrative);
    REQUIRE(serializedRequest.patch.resumeSeed == detailedSeed);
    REQUIRE(serializedRequest.patch.keyFiles == serializedRecord.packet.keyFiles);
    REQUIRE(serializedRequest.patch.nextActions == serializedRecord.packet.nextActions);
    REQUIRE(serializedRequest.patch.blockers == serializedRecord.packet.blockers);
    REQUIRE(serializedRequest.patch.decisions == serializedRecord.packet.decisions);
    REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == serializedRecord.packet.id.value());

    std::size_t mentionCase{};
    for (const auto field : {"narrative", "resume_seed"}) {
        for (const auto evidence : {
                 "<br>No filler or placeholder text is present; unverified claims are marked as such.",
                 " The native validation error named \"lorem ipsum\" and \"placeholder\"; this records the error as evidence.",
                 " User constraint: do not use lorem ipsum or placeholder filler to pad continuity text."}) {
            auto evidencePacket = completeAuto;
            evidencePacket[field] = completeAuto.at(field).get<std::string>() + evidence;
            const auto callsBeforeEvidence = legacyContinuity.handoffCalls();
            auto evidenceCall = authorize("session_handoff", Domain::ToolEffect::Write,
                Json{{"packet_json", evidencePacket.dump()}}.dump(),
                "packet-json-filler-mention-" + std::to_string(++mentionCase));
            const auto evidenceResult = take(adapter->handle(evidenceCall, authority, context));
            REQUIRE(evidenceResult.receipt.ok);
            REQUIRE(legacyContinuity.handoffCalls() == callsBeforeEvidence + 1U);
            REQUIRE(legacyContinuity.lastHandoffRequest()->patch.narrative ==
                evidencePacket.at("narrative").get<std::string>());
            REQUIRE(legacyContinuity.lastHandoffRequest()->patch.resumeSeed ==
                evidencePacket.at("resume_seed").get<std::string>());
            REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == serializedRecord.packet.id.value());
        }
    }
    visibleStatusPayload = "{}";

    legacyContinuity.setPersistOutcome({recoveredRecord, true});
    auto publishHandoffCall = authorize(
        "session_handoff", Domain::ToolEffect::Write,
        R"({"goal":"Continue the adapter test","narrative":"Recovered context","next_actions":["Run the recovered tool"]})",
        "publish-handoff");
    const auto publishedHandoffResult = take(adapter->handle(publishHandoffCall, authority, context));
    REQUIRE(publishedHandoffResult.contextPersistence == handoffId);
    REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == handoffId.value());

    const auto manualHandoffCalls = legacyContinuity.handoffCalls();
    const std::string manualNarrative = "Lorem ipsum draft text retained for manual compatibility.";
    const std::string manualSeed = "PLACEHOLDER manual draft.";
    auto manualFillerCall = authorize("session_handoff", Domain::ToolEffect::Write,
        Json{{"narrative", manualNarrative}, {"resume_seed", manualSeed}}.dump(),
        "manual-filler-compatibility");
    REQUIRE(adapter->handle(manualFillerCall, authority, context));
    REQUIRE(legacyContinuity.handoffCalls() == manualHandoffCalls + 1U);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.narrative == manualNarrative);
    REQUIRE(legacyContinuity.lastHandoffRequest()->patch.resumeSeed == manualSeed);

    const auto manualPaddingCalls = legacyContinuity.handoffCalls();
    auto manualPaddingCall = authorize("session_handoff", Domain::ToolEffect::Write,
        Json{{"resume_seed", std::string(256U, 'a')}, {"next_actions", Json::array({"continue"})}}.dump(),
        "manual-padding-compatibility");
    REQUIRE(adapter->handle(manualPaddingCall, authority, context));
    REQUIRE(legacyContinuity.handoffCalls() == manualPaddingCalls + 1U);

    // A successor checkpoint must preserve the finalized resume pointer.
    legacyContinuity.setPersistOutcome({pendingRecord, false});
    auto successorCheckpointCall = authorize(
        "session_checkpoint", Domain::ToolEffect::Write,
        R"({"goal":"Successor work"})", "successor-checkpoint");
    REQUIRE(adapter->handle(successorCheckpointCall, authority, context));
    REQUIRE(take(legacyMemory.get({resumePointer}, context)).note->body == handoffId.value());
    const auto resumedBootstrap = take(adapter->bootstrapInstructions(projectId, root, context));
    REQUIRE(resumedBootstrap.find("Recovered context") != std::string::npos);
    REQUIRE(resumedBootstrap.find("Run the recovered tool") != std::string::npos);
    REQUIRE(resumedBootstrap.find(handoffId.value()) != std::string::npos);
    REQUIRE(resumedBootstrap.find("call context_get through forge-conductor or forge-conductor-fallback") !=
        std::string::npos);
    REQUIRE(resumedBootstrap.find("CLU connector receives this packet as policy context") !=
        std::string::npos);
    REQUIRE(resumedBootstrap.find("Before continuing, call context_get with") == std::string::npos);
    auto oversizedRecord = recoveredRecord;
    oversizedRecord.packet.resumeSeed = std::string(40U * 1024U, 'x');
    legacyContinuity.setGetOutcome({oversizedRecord, true});
    const auto oversizedBootstrap = take(adapter->bootstrapInstructions(projectId, root, context));
    REQUIRE(oversizedBootstrap.size() < 32U * 1024U);
    REQUIRE(oversizedBootstrap.find("without truncation") != std::string::npos);
    auto checkpointRecord = recoveredRecord;
    checkpointRecord.packet.resumeReady = false;
    legacyContinuity.setGetOutcome({checkpointRecord, true});
    REQUIRE(take(adapter->bootstrapInstructions(projectId, root, context))
        .find("Recovered context") == std::string::npos);
    legacyContinuity.setGetOutcome({recoveredRecord, true});
    const auto adoptedProjectId = parse<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000099");
    clientWorkspaceContext.setAdoption(Domain::ClientWorkspaceAdoption{
        Domain::ClientWorkspaceSnapshot{
            clientId,
            adoptedProjectId,
            secondaryRoot,
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
    REQUIRE(legacyContinuity.lastGetRequest()->handoffId == handoffId);
    REQUIRE(contextPayload.at("successor_session_id") == clientId.value());
    REQUIRE(contextPayload.at("session_kind") == "mcp_connection");
    REQUIRE(contextPayload.at("workspace_adopted") == secondaryRoot.value());
    REQUIRE(observedWorkspace.has_value());
    REQUIRE(observedWorkspace->first == adoptedProjectId);
    REQUIRE(observedWorkspace->second == secondaryRoot);
    REQUIRE(contextPayload.at("workspace_project_id") == adoptedProjectId.value());
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
    REQUIRE(fileSystem.calls() == 1U);
    REQUIRE(fileSystem.lastCapture().has_value());
    REQUIRE(fileSystem.lastCapture()->primary.canonicalPath().value() ==
            "D:/workspace\\notes.txt");

    auto secondAliasReadCall = authorize(
        "fs_read",
        Domain::ToolEffect::Read,
        R"({"path":"E:/workspace-secondary/notes.txt"})",
        "request-fs-read-second-alias");
    auto secondAliasRead = adapter->handle(
        secondAliasReadCall, authority, context);
    REQUIRE(secondAliasRead);
    REQUIRE(secondAliasRead.value().continuityObservation.has_value());
    REQUIRE(secondAliasRead.value().continuityObservation->path ==
            std::optional<Domain::PathText>{take(Domain::PathText::create(
                "E:/workspace-secondary/notes.txt"))});
    REQUIRE(secondAliasRead.value().continuityObservation->baseDirectory ==
            std::optional<Domain::PathText>{secondaryRoot});
    REQUIRE(fileSystem.calls() == 2U);
    REQUIRE(fileSystem.lastCapture().has_value());
    REQUIRE(fileSystem.lastCapture()->primary.canonicalPath().value() ==
            "E:/workspace-secondary/notes.txt");

    fileSystem.writeFileResult.set(Domain::Result<void>::success());
    fileSystem.failNextWrite(Domain::makeError(
        Domain::ErrorCodes::RecordNotFound,
        "The target does not exist."));
    const auto callsBeforeCreateWrite = fileSystem.calls();
    auto createWriteCall = authorize(
        "fs_write",
        Domain::ToolEffect::Write,
        R"({"content":"created","path":"created.txt"})",
        "request-fs-write-create");
    auto createWrite = adapter->handle(createWriteCall, authority, context);
    REQUIRE(createWrite);
    REQUIRE(Json::parse(createWrite.value().canonicalPayload).at(
                "bytes_written") == 7U);
    REQUIRE(fileSystem.calls() == callsBeforeCreateWrite + 2U);
    REQUIRE(fileSystem.lastCapture().has_value());
    REQUIRE(fileSystem.lastCapture()->call == Fakes::FileSystemCall::WriteFile);
    REQUIRE(fileSystem.lastCapture()->primary.access() ==
            Domain::FileAccess::Create);
    REQUIRE(fileSystem.lastCapture()->primary.canonicalPath().value() ==
            "D:/workspace\\created.txt");

    auto mismatchedCwdCall = authorize(
        "git_status",
        Domain::ToolEffect::Read,
        R"({"cwd":"F:/unregistered-project"})",
        "request-git-mismatched-cwd");
    auto mismatchedCwd = adapter->handle(
        mismatchedCwdCall, authority, context);
    REQUIRE(!mismatchedCwd);
    REQUIRE(mismatchedCwd.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(readPayload.at("line_count") == 1);
    REQUIRE(readPayload.at("next_offset") == 3);
    REQUIRE(readPayload.at("has_more") == true);
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
    REQUIRE(largeReadResult.value().canonicalPayload.size() <= 32U * 1024U);
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
    REQUIRE(continuedReadResult.value().canonicalPayload.size() <= 32U * 1024U);
    REQUIRE(largeReadPayload.at("start_line") == 1U);
    REQUIRE(largeReadPayload.at("end_line") == 1U);
    REQUIRE(largeReadPayload.at("line_count") == 1U);
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
    REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Search) == 4U);
    REQUIRE(projectMemory.lastContext().has_value());
    REQUIRE(projectMemory.lastContext()->deadline ==
            Domain::MonotonicTimePoint{} +
                Domain::MaximumProjectMemoryDeadline);
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
    REQUIRE(projectMemory.lastContext().has_value());
    REQUIRE(projectMemory.lastContext()->deadline ==
            Domain::MonotonicTimePoint{} + 5s);
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
    REQUIRE(!previewImport);
    REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Import) == 0U);

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
    REQUIRE(!committedImport);
    REQUIRE(projectMemory.callCount(Fakes::ProjectMemoryCall::Import) == 0U);

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

    const auto requireCapturedGitAuthority = [&] (
        const Fakes::GitServiceCall expectedCall) {
        REQUIRE(git.lastCapture().has_value());
        REQUIRE(git.lastCapture()->call == expectedCall);
        REQUIRE(git.lastCapture()->authority.authorityId() ==
                authority.authorityId());
        REQUIRE(git.lastCapture()->authority.projectId() == authority.projectId());
        REQUIRE(git.lastCapture()->authority.callerId() == authority.callerId());
        REQUIRE(git.lastCapture()->authority.trustedRoots() ==
                authority.trustedRoots());
        REQUIRE(git.lastCapture()->authority.intent() == authority.intent());
        REQUIRE(git.lastCapture()->authority.grants() == authority.grants());
        REQUIRE(git.lastCapture()->authority.denials() == authority.denials());
        REQUIRE(git.lastCapture()->authority.shellEnabled() ==
                authority.shellEnabled());
        REQUIRE(git.lastCapture()->authority.generation() ==
                authority.generation());
    };

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
    requireCapturedGitAuthority(Fakes::GitServiceCall::Status);

    git.diffResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0, "diff", "", false, false,
            false, false, true, 1ms}));
    auto gitDiffCall = authorize(
        "git_diff",
        Domain::ToolEffect::Read,
        R"({"staged":true})",
        "request-git-authority-diff");
    auto gitDiff = adapter->handle(gitDiffCall, authority, context);
    REQUIRE(gitDiff);
    requireCapturedGitAuthority(Fakes::GitServiceCall::Diff);

    git.logResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0, "log", "", false, false,
            false, false, true, 1ms}));
    auto gitLogCall = authorize(
        "git_log",
        Domain::ToolEffect::Read,
        R"({"limit":3})",
        "request-git-authority-log");
    auto gitLog = adapter->handle(gitLogCall, authority, context);
    REQUIRE(gitLog);
    requireCapturedGitAuthority(Fakes::GitServiceCall::Log);

    git.addResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0, "added", "", false, false,
            false, false, true, 1ms}));
    auto gitAddCall = authorize(
        "git_add",
        Domain::ToolEffect::Write,
        R"({"path":"-A"})",
        "request-git-authority-add");
    auto gitAdd = adapter->handle(gitAddCall, authority, context);
    REQUIRE(gitAdd);
    requireCapturedGitAuthority(Fakes::GitServiceCall::Add);

    git.commitResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0, "committed", "", false, false,
            false, false, true, 1ms}));
    auto gitCommitCall = authorize(
        "git_commit",
        Domain::ToolEffect::Write,
        "{}",
        "request-git-authority-commit");
    auto gitCommit = adapter->handle(gitCommitCall, authority, context);
    REQUIRE(gitCommit);
    requireCapturedGitAuthority(Fakes::GitServiceCall::Commit);

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

    Domain::ShellJobSnapshot trackedJob{"job-runtime-test", Domain::ShellJobState::Running,
        "Write-Output long-test", root.value(), 1800U, std::nullopt, std::nullopt, 10ms};
    shell.startJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    shell.startProcessResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    {
        const auto starts = shell.processStartCalls;
        const auto sourceArguments = Json{{"path", root.value() + "/verification-env"}, {"python_path", "C:/Python/python.exe"}};
        const auto workspaceDenied = adapter->handle(authorizeFor("verification_env_create", Domain::ToolEffect::Write,
            sourceArguments.dump(), "verification-workspace-source-denied", shellAuthority), shellAuthority, context);
        REQUIRE(!workspaceDenied && workspaceDenied.error().code == Domain::ErrorCodes::Unauthorized);
        REQUIRE(!workspaceAuthority.lastExcludedSubtree);
        REQUIRE(shell.processStartCalls == starts);
        workspaceAuthority.enableHost(shellAuthority, root);
        const auto hostToken = take(workspaceAuthority.authorityFor(projectId, context));
        const auto hostDenied = adapter->handle(authorizeFor("verification_env_create", Domain::ToolEffect::Write,
            sourceArguments.dump(), "verification-host-source-denied", hostToken), hostToken, context);
        REQUIRE(!hostDenied && hostDenied.error().code == Domain::ErrorCodes::Unauthorized);
        REQUIRE(workspaceAuthority.lastExcludedSubtree == std::optional<Domain::PathText>{root});
        REQUIRE(shell.processStartCalls == starts);
        const auto outsideArguments = Json{{"path", "D:/workspace-other/verification-env"}, {"python_path", "C:/Python/python.exe"}};
        const auto outside = take(adapter->handle(authorizeFor("verification_env_create", Domain::ToolEffect::Write,
            outsideArguments.dump(), "verification-host-outside-admitted", hostToken), hostToken, context));
        REQUIRE(outside.receipt.ok);
        REQUIRE(workspaceAuthority.lastExcludedSubtree == std::optional<Domain::PathText>{root});
        REQUIRE(shell.processStartCalls == starts + 1U);
        REQUIRE(shell.lastJobRequest && shell.lastJobRequest->arguments.at(3) == "D:/workspace-other/verification-env");
        workspaceAuthority.restoreWorkspace();
    }
    {
        auto testJob = trackedJob;
        testJob.cmakeTest = Domain::CMakeTestMetadata{root.value(), false, {}, {}, {},
            root.value() + "/owned.ctest.xml"};
        shell.startCMakeTestResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(testJob));
        const auto testRun = [&](Json arguments, const auto& selectedAuthority) {
            return adapter->handle(authorizeFor("cmake_test_run", Domain::ToolEffect::Write,
                arguments.dump(), "cmake-local-run", selectedAuthority), selectedAuthority, context);
        };
        auto started = take(testRun(Json{{"build_dir", root.value()}}, shellAuthority));
        auto value = Json::parse(started.canonicalPayload);
        REQUIRE(started.receipt.ok && value.at("done") == false);
        REQUIRE(value.at("cmake_test").at("build_result").is_null());
        REQUIRE(value.at("cmake_test").at("test_result").is_null());
        REQUIRE(value.at("cmake_test").at("counts").is_null());
        REQUIRE(shell.lastCMakeTestRequest->timeout == 1800s);
        REQUIRE(!shell.lastCMakeTestRequest->build);
        REQUIRE(shell.lastCMakeTestRequest->buildDirectory == root);
        started = take(testRun(Json{{"build_dir", root.value()}, {"mode", "build_and_test"},
            {"target", "ProductAll"}, {"filter", "Core.UnitTests"}, {"config", "Release"}, {"timeout_sec", 17}}, shellAuthority));
        REQUIRE(started.receipt.ok);
        REQUIRE(shell.lastCMakeTestRequest->build);
        REQUIRE(shell.lastCMakeTestRequest->target == "ProductAll");
        REQUIRE(shell.lastCMakeTestRequest->filter == "Core.UnitTests");
        REQUIRE(shell.lastCMakeTestRequest->configuration == "Release");
        REQUIRE(shell.lastCMakeTestRequest->timeout == 17s);
        const auto startCount = shell.cmakeTestStartCalls;
        for (const auto& arguments : std::vector<Json>{Json::object(), Json{{"build_dir", ""}},
                Json{{"build_dir", root.value()}, {"target", "ProductAll"}},
                Json{{"build_dir", root.value()}, {"mode", "other"}},
                Json{{"build_dir", root.value()}, {"timeout_sec", 0}},
                Json{{"build_dir", root.value()}, {"timeout_sec", 3601}},
                Json{{"build_dir", root.value()}, {"timeout_sec", 1.25}},
                Json{{"build_dir", root.value()}, {"extra", true}}}) {
            REQUIRE(!testRun(arguments, shellAuthority));
        }
        REQUIRE(!testRun(Json{{"build_dir", "Z:/outside/build"}}, shellAuthority));
        REQUIRE(shell.cmakeTestStartCalls == startCount);

        testJob.state = Domain::ShellJobState::Failed;
        testJob.cmakeTest->buildRequested = true;
        testJob.cmakeTest->buildResult = Domain::ProcessResult{7, "build output", "build failed"};
        shell.getCMakeTestResult.set(Domain::Result<Domain::CMakeTestRunStatus>::success(
            Domain::CMakeTestRunStatus{testJob}));
        const auto testStatus = [&](Json arguments) {
            arguments["job_id"] = testJob.jobId;
            return adapter->handle(authorize("cmake_test_status", Domain::ToolEffect::Read,
                arguments.dump(), "cmake-local-status"), authority, context);
        };
        auto failedBuild = take(testStatus(Json::object()));
        value = Json::parse(failedBuild.canonicalPayload);
        REQUIRE(failedBuild.receipt.ok);
        REQUIRE(value.at("done") == true && value.at("state") == "failed");
        REQUIRE(value.at("cmake_test").at("build_result").at("ok") == false);
        REQUIRE(value.at("cmake_test").at("build_result").at("exit_code") == 7);
        REQUIRE(value.at("cmake_test").at("test_result").is_null());
        REQUIRE(value.at("cmake_test").at("counts").is_null());
        REQUIRE(!value.at("cmake_test").at("build_result").contains("stdout"));
        REQUIRE(shell.lastFailureOffset == 0U && shell.lastMaximumFailures == 16U);

        testJob.cmakeTest->buildRequested = false;
        testJob.cmakeTest->buildResult.reset();
        testJob.cmakeTest->testResult = Domain::ProcessResult{8, "CTest output", ""};
        testJob.cmakeTest->reportUnverified = true;
        testJob.cmakeTest->reportError = Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
            "The produced test report exceeded its capture bound.");
        shell.getCMakeTestResult.set(Domain::Result<Domain::CMakeTestRunStatus>::success(
            Domain::CMakeTestRunStatus{testJob}));
        value = Json::parse(take(testStatus(Json::object())).canonicalPayload);
        REQUIRE(value.at("cmake_test").at("report_unverified") == true);
        REQUIRE(value.at("cmake_test").at("report_sha256").is_null());
        REQUIRE(value.at("cmake_test").at("counts").is_null());
        REQUIRE(value.at("cmake_test").at("report_error").at("code").get<std::string>() == Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(value.at("cmake_test").at("test_result").at("exit_code") == 8);
        testJob.cmakeTest->reportUnverified = false;
        testJob.cmakeTest->reportError.reset();

        testJob.cmakeTest->buildResult.reset();
        testJob.cmakeTest->testResult = Domain::ProcessResult{8, "CTest output", ""};
        testJob.cmakeTest->counts = Domain::CMakeTestCounts{4U, 1U, 2U, 1U, 0U};
        testJob.cmakeTest->reportSha256 = packageRevision.value();
        testJob.cmakeTest->reportBytes = 983U;
        shell.getCMakeTestResult.set(Domain::Result<Domain::CMakeTestRunStatus>::success(
            Domain::CMakeTestRunStatus{testJob, {{"second failure", "fail", "explicit message", "actual output", false}},
                1U, 2U, 2U, false}));
        auto failedTests = take(testStatus(Json{{"failure_offset", 1}, {"max_failures", 1}}));
        value = Json::parse(failedTests.canonicalPayload);
        REQUIRE(failedTests.receipt.ok);
        REQUIRE(value.at("cmake_test").at("test_result").at("exit_code") == 8);
        REQUIRE(value.at("cmake_test").at("counts").at("failed") == 2U);
        REQUIRE(value.at("cmake_test").at("counts").at("passed") == 1U);
        REQUIRE(value.at("cmake_test").at("report_sha256") == packageRevision.value());
        REQUIRE(value.at("failures").size() == 1U);
        REQUIRE(value.at("failures").at(0).at("output") == "actual output");
        REQUIRE(!value.at("failures").at(0).contains("expected"));
        REQUIRE(!value.at("failures").at(0).contains("actual"));
        REQUIRE(value.at("failure_offset") == 1U && value.at("next_failure_offset") == 2U);
        REQUIRE(value.at("has_more") == false);
        REQUIRE(shell.lastFailureOffset == 1U && shell.lastMaximumFailures == 1U);
        const auto statusCount = shell.cmakeTestStatusCalls;
        REQUIRE(!testStatus(Json{{"failure_offset", -1}}));
        REQUIRE(!testStatus(Json{{"max_failures", 0}}));
        REQUIRE(!testStatus(Json{{"max_failures", 33}}));
        REQUIRE(shell.cmakeTestStatusCalls == statusCount);
        shell.getCMakeTestResult.set(Domain::Result<Domain::CMakeTestRunStatus>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "The sealed test report changed.")));
        const auto changedReport = testStatus(Json::object());
        REQUIRE(!changedReport && changedReport.error().code == Domain::ErrorCodes::IntegrityFailure);
    }
    auto startJobCall = authorizeFor("shell_job_start", Domain::ToolEffect::Write,
        R"({"command":"Write-Output long-test","cwd":"D:/workspace"})", "start-shell-job", shellAuthority);
    auto jobStart = take(adapter->handle(startJobCall, shellAuthority, context));
    auto jobPayload = Json::parse(jobStart.canonicalPayload);
    REQUIRE(jobStart.receipt.ok);
    REQUIRE(jobPayload.at("job_id") == trackedJob.jobId);
    REQUIRE(jobPayload.at("state") == "running");
    REQUIRE(jobPayload.at("done") == false);
    REQUIRE(jobPayload.at("result").is_null());
    REQUIRE(jobPayload.at("alive").is_null());
    REQUIRE(shell.lastJobRequest->timeout == 1800s);
    REQUIRE(!shell.lastJobRequest->managedJob);
    const auto starts = shell.jobStartCalls;
    for (const auto& invalid : {R"({"command":"test","timeout_sec":3601})",
                                R"({"command":"test","timeout_sec":0})",
                                R"({"command":"test","extra":"value"})"}) {
        auto invalidCall = authorizeFor("shell_job_start", Domain::ToolEffect::Write, invalid,
            "invalid-shell-job", shellAuthority);
        REQUIRE(!adapter->handle(invalidCall, shellAuthority, context));
    }
    REQUIRE(shell.jobStartCalls == starts);
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    auto pollJobCall = authorize("shell_job_status", Domain::ToolEffect::Read,
        R"({"job_id":"job-runtime-test"})", "poll-shell-job");
    auto pollJob = take(adapter->handle(pollJobCall, authority, context));
    REQUIRE(pollJob.receipt.ok);
    REQUIRE(Json::parse(pollJob.canonicalPayload).at("poll_after_sec") == 5);
    REQUIRE(shell.lastJobId == trackedJob.jobId);
    trackedJob.processId = 42U;
    trackedJob.processCreationTime = 123U;
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    pollJob = take(adapter->handle(pollJobCall, authority, context));
    REQUIRE(Json::parse(pollJob.canonicalPayload).at("alive").is_null());
    trackedJob.processAlive = false;
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    pollJob = take(adapter->handle(pollJobCall, authority, context));
    jobPayload = Json::parse(pollJob.canonicalPayload);
    REQUIRE(jobPayload.at("alive") == false);
    REQUIRE(jobPayload.at("done") == false);
    trackedJob.state = Domain::ShellJobState::Failed;
    trackedJob.processAlive = false;
    trackedJob.result = Domain::ProcessResult{7, "captured final output", "test failure"};
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    pollJob = take(adapter->handle(pollJobCall, authority, context));
    jobPayload = Json::parse(pollJob.canonicalPayload);
    REQUIRE(pollJob.receipt.ok); // The status read succeeded; the command failed.
    REQUIRE(jobPayload.at("done") == true);
    REQUIRE(jobPayload.at("alive") == false);
    REQUIRE(jobPayload.at("result").at("ok") == false);
    REQUIRE(jobPayload.at("result").at("exit_code") == 7);
    REQUIRE(jobPayload.at("result").at("stdout") == "captured final output");
    trackedJob.result->stdoutUtf8 = std::string(80'000U, '\x01');
    trackedJob.result->stderrUtf8 = std::string(20'000U, '\x01');
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    auto escapedStatus = take(adapter->handle(pollJobCall, authority, context));
    REQUIRE(escapedStatus.canonicalPayload.size() < 128U * 1024U);
    const auto escapedOutput = Json::parse(escapedStatus.canonicalPayload).at("result");
    REQUIRE(escapedOutput.at("stdout") == std::string(16U * 1024U, '\x01'));
    REQUIRE(escapedOutput.at("stderr") == std::string(4U * 1024U, '\x01'));
    REQUIRE(escapedOutput.at("stdout_captured_bytes") == 80'000U);
    REQUIRE(escapedOutput.at("stderr_captured_bytes") == 20'000U);
    REQUIRE(escapedOutput.at("stdout_response_truncated") == true);
    REQUIRE(escapedOutput.at("stderr_response_truncated") == true);
    REQUIRE(escapedOutput.at("stdout_capture_truncated") == false);
    REQUIRE(escapedOutput.at("stderr_capture_truncated") == false);
    REQUIRE(escapedOutput.at("stdout_truncated") == true);
    trackedJob.result->stdoutUtf8 = std::string(16U * 1024U - 1U, 'x') + "\xe2\x82\xac";
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    auto utf8Preview = Json::parse(take(adapter->handle(pollJobCall, authority, context)).canonicalPayload);
    REQUIRE(utf8Preview.at("result").at("stdout") == std::string(16U * 1024U - 1U, 'x'));
    trackedJob.result->stdoutUtf8 = std::string(80'000U, '\x01');
    shell.listJobsResult.set(Domain::Result<std::vector<Domain::ShellJobSnapshot>>::success({trackedJob}));
    auto listJobCall = authorize("shell_job_list", Domain::ToolEffect::Read, "{}", "list-shell-job");
    auto listedJob = Json::parse(take(adapter->handle(listJobCall, authority, context)).canonicalPayload);
    REQUIRE(listedJob.at("jobs").size() == 1U);
    REQUIRE(listedJob.at("lifetime") == "manager_process");
    REQUIRE(listedJob.at("pid") == 42U);
    REQUIRE(!listedJob.at("jobs").at(0).at("result").contains("stdout"));
    REQUIRE(!listedJob.at("jobs").at(0).contains("args"));
    trackedJob.command = std::string(255U, 'c') + "\xe2\x82\xac" + std::string(3'838U, '\x01');
    trackedJob.arguments.assign(4U, std::string(3'500U, '\x01'));
    shell.listJobsResult.set(Domain::Result<std::vector<Domain::ShellJobSnapshot>>::success(
        std::vector<Domain::ShellJobSnapshot>(32U, trackedJob)));
    auto boundedList = take(adapter->handle(listJobCall, authority, context));
    REQUIRE(boundedList.canonicalPayload.size() < 128U * 1024U);
    auto boundedJobs = Json::parse(boundedList.canonicalPayload).at("jobs");
    REQUIRE(boundedJobs.size() == 32U);
    for (const auto& summary : boundedJobs) {
        REQUIRE(!summary.contains("args"));
        REQUIRE(summary.at("command_truncated") == true);
        REQUIRE(summary.at("command") == std::string(255U, 'c'));
        REQUIRE(!summary.at("result").contains("stdout"));
    }
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    auto detailedJob = Json::parse(take(adapter->handle(pollJobCall, authority, context)).canonicalPayload);
    REQUIRE(detailedJob.at("args") == trackedJob.arguments);
    REQUIRE(detailedJob.at("command") == trackedJob.command);
    trackedJob.state = Domain::ShellJobState::Cancelled;
    shell.cancelJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::success(trackedJob));
    auto cancelJobCall = authorize("shell_job_cancel", Domain::ToolEffect::Write,
        R"({"job_id":"job-runtime-test"})", "cancel-shell-job");
    auto cancelledJob = take(adapter->handle(cancelJobCall, authority, context));
    REQUIRE(Json::parse(cancelledJob.canonicalPayload).at("state") == "cancelled");
    shell.getJobResult.set(Domain::Result<Domain::ShellJobSnapshot>::failure(
        Domain::makeError(Domain::ErrorCodes::RecordNotFound, "Unknown job")));
    auto missingJob = adapter->handle(pollJobCall, authority, context);
    REQUIRE(!missingJob);
    REQUIRE(missingJob.error().code == Domain::ErrorCodes::RecordNotFound);

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
            2U,
            3U}));

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
    take(projectRegistry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        projectId,
        "Continuity integration project",
        std::optional<std::string>{"continuity-integration-project"},
        {root}}));
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
            hasher,
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

    auto firstProgress = invoke("fs_read", R"({"path":"notes.txt"})");
    REQUIRE(firstProgress);
    REQUIRE(firstProgress.value().receipt.ok);
    REQUIRE(!Json::parse(firstProgress.value().canonicalPayload).contains(
        "handoff_required"));

    auto repeatedProgress = invoke("fs_read", R"({"path":"notes.txt"})");
    REQUIRE(repeatedProgress);
    REQUIRE(repeatedProgress.value().receipt.ok);
    const auto repeatedPayload = Json::parse(
        repeatedProgress.value().canonicalPayload);
    REQUIRE(repeatedPayload.at("handoff_id") == handoffId.value());
    REQUIRE(repeatedPayload.at("handoff_required") == true);

    auto blockedLoop = invoke("fs_read", R"({"path":"notes.txt"})");
    REQUIRE(blockedLoop);
    REQUIRE(!blockedLoop.value().receipt.ok);
    REQUIRE(Json::parse(blockedLoop.value().canonicalPayload).at("code") ==
            "identical_call_loop");
    REQUIRE(legacyContinuity.automaticCalls() == 0U);
    REQUIRE(legacyContinuity.budgetCalls() == 2U);
    REQUIRE(legacyContinuity.lastBudgetReason().has_value());
    REQUIRE(legacyContinuity.lastBudgetReason()->starts_with(
        "identical_call_loop"));

    static_cast<void>(take(legacyMemory.set({"continuity/project/" + projectId.value(), handoffId.value(), {}}, authorityContext)));
    auto blockedForgeStatus = invoke("forge_status", "{}");
    REQUIRE(blockedForgeStatus);
    const auto blockedPayload = Json::parse(
        blockedForgeStatus.value().canonicalPayload);
    const auto& blockedAutomatic = blockedPayload.at("auto_continuity");
    REQUIRE(blockedAutomatic.at("enabled") == true);
    REQUIRE(blockedAutomatic.at("blocked") == false);
    REQUIRE(blockedAutomatic.at("handoff_pending") == true);
    REQUIRE(blockedAutomatic.at("resume_packet_ready") == false);
    REQUIRE(blockedAutomatic.at("resume_packet_id").is_null());
    REQUIRE(blockedAutomatic.at("readback_confirmed") == false);
    REQUIRE(blockedAutomatic.at("readback_handoff_id").is_null());
    REQUIRE(blockedAutomatic.at("readback_scope") == "this_mcp_client_confirmed_context_get");
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
    REQUIRE(!afterStale.blocked);
    REQUIRE(afterStale.handoffPending);
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
    REQUIRE(guard->snapshot(clientId).readbackHandoffId == std::optional<std::string>{handoffId.value()});
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
    REQUIRE(resumedAutomatic.at("blocked") == false);
    REQUIRE(resumedAutomatic.at("resume_packet_ready") == false);
    REQUIRE(resumedAutomatic.at("resume_packet_id").is_null());
    REQUIRE(resumedAutomatic.at("readback_confirmed") == false);
    REQUIRE(resumedAutomatic.at("readback_handoff_id") == handoffId.value());
    REQUIRE(resumedAutomatic.at("readback_scope") == "this_mcp_client_confirmed_context_get");
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
    REQUIRE(audit.eventCount() == 7U);

    auto modelRecord = recoveredRecord;
    const auto modelHandoffId = parse<Domain::LegacyHandoffId>("model-ready-current-packet");
    modelRecord.packet.id = modelHandoffId;
    modelRecord.packet.source = Domain::LegacyHandoffSource::Model;
    legacyContinuity.setGetOutcome({modelRecord, true});
    static_cast<void>(take(legacyMemory.set(
        {"continuity/project/" + projectId.value(), modelHandoffId.value(), {}}, authorityContext)));
    const auto readyStatus = Json::parse(take(invoke("get_forge_status", "{}")).canonicalPayload);
    const auto& readyAutomatic = readyStatus.at("auto_continuity");
    REQUIRE(readyAutomatic.at("resume_packet_ready") == true);
    REQUIRE(readyAutomatic.at("resume_packet_id") == modelHandoffId.value());
    REQUIRE(readyAutomatic.at("readback_confirmed") == false);
    REQUIRE(readyAutomatic.at("readback_handoff_id") == handoffId.value());
    const auto modelReadback = take(invoke("context_get",
        Json{{"handoff_id", modelHandoffId.value()}, {"resume_ready", true}}.dump()));
    REQUIRE(modelReadback.receipt.ok);
    REQUIRE(Json::parse(modelReadback.canonicalPayload).at("found") == true);
    const auto confirmedStatus = Json::parse(take(invoke("get_forge_status", "{}")).canonicalPayload);
    const auto& confirmedAutomatic = confirmedStatus.at("auto_continuity");
    REQUIRE(confirmedAutomatic.at("resume_packet_ready") == true);
    REQUIRE(confirmedAutomatic.at("resume_packet_id") == modelHandoffId.value());
    REQUIRE(confirmedAutomatic.at("readback_confirmed") == true);
    REQUIRE(confirmedAutomatic.at("readback_handoff_id") == modelHandoffId.value());
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
