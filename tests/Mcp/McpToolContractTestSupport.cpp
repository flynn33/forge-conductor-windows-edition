#include "Mcp/McpToolContractTestSupport.h"

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IForgeStatusRepository.h"
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/IMcpClientWorkspaceContext.h"
#include "ForgeConductor/Contracts/IPathGlobService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
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
#include "Fakes/RecordingContinuityCoordinator.h"
#include "Fakes/RecordingProjectMemoryService.h"
#include "Fakes/ShellServiceFake.h"
#include "Fakes/TextSearchServiceFake.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::McpContract {
namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Mcp = ForgeConductor::Mcp;

using Json = nlohmann::json;
using namespace std::chrono_literals;

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::Error databaseBusy()
{
    return Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy,
        "The deterministic contract dependency is busy.",
        true);
}

template <typename T>
[[nodiscard]] Domain::Result<T> resultFor(
    const DependencyMode mode,
    T value)
{
    if (mode == DependencyMode::DatabaseBusy) {
        return Domain::Result<T>::failure(databaseBusy());
    }
    return Domain::Result<T>::success(std::move(value));
}

[[nodiscard]] Domain::Result<void> voidResultFor(
    const DependencyMode mode)
{
    if (mode == DependencyMode::DatabaseBusy) {
        return Domain::Result<void>::failure(databaseBusy());
    }
    return Domain::Result<void>::success();
}

class EffectLedger final {
public:
    void record(const std::string_view name) noexcept
    {
        try {
            effects_.emplace_back(name);
        } catch (...) {
        }
    }

    [[nodiscard]] std::vector<std::string> sorted() const
    {
        auto result = effects_;
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    std::vector<std::string> effects_;
};

class PassThroughInvocationGuard final
    : public Contracts::IToolInvocationGuard {
public:
    [[nodiscard]] Domain::Result<Domain::ToolInvocationAdmission> beforeInvoke(
        const Domain::ToolCallRequest&,
        const Domain::ToolDescriptor&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ToolInvocationAdmission>::success({});
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> afterInvoke(
        const Domain::ToolCallRequest&,
        const Domain::ToolDescriptor&,
        Domain::Result<Domain::ToolCallOutcome> outcome,
        const Domain::OperationContext&) noexcept override
    {
        return outcome;
    }

    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}
};

class MatrixReportInspector final
    : public Contracts::IAgentCompletionReportInspector {
public:
    MatrixReportInspector(EffectLedger& effects, const DependencyMode mode)
        : effects_{effects}, mode_{mode}
    {
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentReportField>> inspect(
        const std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("report_inspector.inspect");
        return resultFor(
            mode_,
            std::vector<Domain::AgentReportField>{
                {"summary", Domain::AgentReportValueKind::String, 4U}});
    }

private:
    EffectLedger& effects_;
    DependencyMode mode_;
};

class MatrixFileTextServices final
    : public Contracts::ITextFileEditService,
      public Contracts::IPathGlobService {
public:
    MatrixFileTextServices(EffectLedger& effects, const DependencyMode mode)
        : effects_{effects}, mode_{mode}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::TextFileEditReport> replaceAll(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        const std::string_view,
        const std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("text_file_editor.replace_all");
        return resultFor(mode_, Contracts::TextFileEditReport{1U, 12U});
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::PathText>> glob(
        const Contracts::AuthorizedPath&,
        const std::string_view,
        const std::size_t,
        const std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("path_glob.glob");
        return resultFor(
            mode_,
            std::vector<Domain::PathText>{
                take(Domain::PathText::create("D:/workspace/src/main.cpp"))});
    }

private:
    EffectLedger& effects_;
    DependencyMode mode_;
};

class MatrixContinuityCodec final
    : public Contracts::IContinuityDocumentCodec {
public:
    MatrixContinuityCodec(
        EffectLedger& effects,
        const DependencyMode mode,
        Domain::ContinuityHandoff baseline)
        : effects_{effects}, mode_{mode}, baseline_{std::move(baseline)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("continuity_codec.encode");
        return resultFor(mode_, Contracts::ContinuityDocument{handoff, "{}"});
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> decode(
        const std::string_view canonicalUtf8,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("continuity_codec.decode");
        return resultFor(
            mode_,
            Contracts::ContinuityDocument{
                baseline_, std::string{canonicalUtf8}});
    }

private:
    EffectLedger& effects_;
    DependencyMode mode_;
    Domain::ContinuityHandoff baseline_;
};

class MatrixContinuityAutomationStatus final
    : public Contracts::IContinuityAutomationStatusSource {
public:
    explicit MatrixContinuityAutomationStatus(EffectLedger& effects)
        : effects_{effects}
    {
    }

    [[nodiscard]] Domain::ContinuityAutomationStatusSnapshot snapshot(
        const Domain::ClientId&) const noexcept override
    {
        effects_.record("continuity_automation.snapshot");
        return {};
    }

private:
    EffectLedger& effects_;
};

class MatrixForgeStatusRepository final
    : public Contracts::IForgeStatusRepository {
public:
    MatrixForgeStatusRepository(EffectLedger& effects, const DependencyMode mode)
        : effects_{effects}, mode_{mode}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ForgeStatusProjection> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("forge_status.snapshot");
        return resultFor(mode_, Domain::ForgeStatusProjection{1U, {}});
    }

    void close() noexcept override {}

private:
    EffectLedger& effects_;
    DependencyMode mode_;
};

class MatrixClientWorkspaceContext final
    : public Contracts::IMcpClientWorkspaceContext {
public:
    MatrixClientWorkspaceContext(
        EffectLedger& effects,
        const DependencyMode mode,
        Domain::ClientWorkspaceAdoption adoption)
        : effects_{effects}, mode_{mode}, adoption_{std::move(adoption)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId&,
        const Domain::LegacyContinuityRecord&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("client_workspace.adopt");
        return resultFor(mode_, adoption_);
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>> snapshot(
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("client_workspace.snapshot");
        return resultFor(mode_, adoption_.snapshot);
    }

    void clear(const Domain::ClientId&) noexcept override {}
    void shutdown() noexcept override {}

private:
    EffectLedger& effects_;
    DependencyMode mode_;
    Domain::ClientWorkspaceAdoption adoption_;
};

class MatrixProjectRegistry final
    : public Contracts::IProjectRegistryRepository {
public:
    MatrixProjectRegistry(
        EffectLedger& effects,
        const DependencyMode mode,
        Domain::ProjectMemoryDescriptor descriptor)
        : effects_{effects}, mode_{mode}, descriptor_{std::move(descriptor)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("project_registry.initialize");
        return resultFor(
            mode_, Domain::ProjectInitialization{descriptor_});
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("project_registry.descriptor");
        return resultFor(mode_, descriptor_);
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>
    list(
        const std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("project_registry.list");
        return resultFor(
            mode_, std::vector<Domain::ProjectMemoryDescriptor>{descriptor_});
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId&,
        const Domain::PathText&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("project_registry.detach_alias");
        return voidResultFor(mode_);
    }

private:
    EffectLedger& effects_;
    DependencyMode mode_;
    Domain::ProjectMemoryDescriptor descriptor_;
};

class MatrixLegacyMemory final
    : public Contracts::ILegacyMemoryService {
public:
    MatrixLegacyMemory(EffectLedger& effects, const DependencyMode mode)
        : effects_{effects}, mode_{mode}
    {
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.set");
        return resultFor(
            mode_,
            Domain::LegacyMemorySetOutcome{
                Domain::MemoryNote{
                    request.key,
                    request.body.value_or(""),
                    request.tags,
                    Domain::UtcTimePoint{},
                    Domain::UtcTimePoint{}},
                true});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.get");
        return resultFor(
            mode_,
            Domain::LegacyMemoryGetOutcome{
                request.key,
                Domain::MemoryNote{
                    request.key,
                    "remembered body",
                    {"contract"},
                    Domain::UtcTimePoint{},
                    Domain::UtcTimePoint{}}});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.list");
        return resultFor(
            mode_,
            Domain::LegacyMemoryListOutcome{
                {Domain::LegacyMemoryNoteProjection{
                    "contract.note",
                    std::nullopt,
                    15U,
                    {"contract"},
                    Domain::UtcTimePoint{},
                    Domain::UtcTimePoint{}}},
                1U});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.remove");
        return resultFor(
            mode_,
            Domain::LegacyMemoryDeleteOutcome{
                request.key, true, true, false});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.search");
        return resultFor(
            mode_,
            Domain::LegacyMemorySearchOutcome{
                request.query.value_or(""),
                {Domain::LegacyMemoryNoteProjection{
                    "contract.note",
                    request.includeBody
                        ? std::optional<std::string>{"remembered body"}
                        : std::nullopt,
                    15U,
                    {"contract"},
                    Domain::UtcTimePoint{},
                    Domain::UtcTimePoint{}}}});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.purge");
        return resultFor(mode_, Domain::LegacyMemoryPurgeOutcome{1U, true});
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_memory.quick_check");
        return voidResultFor(mode_);
    }

    void shutdown() noexcept override {}

private:
    EffectLedger& effects_;
    DependencyMode mode_;
};

[[nodiscard]] Domain::LegacyContinuityRecord legacyContinuityRecord()
{
    return Domain::LegacyContinuityRecord{
        Domain::LegacyHandoffPacket{
            parse<Domain::LegacyHandoffId>("contract-handoff"),
            Domain::LegacyContinuityLimits::SchemaVersion,
            Domain::UtcTimePoint{},
            Domain::UtcTimePoint{},
            Domain::LegacyHandoffSource::Model,
            true,
            std::optional<std::string>{"contract"},
            parse<Domain::ClientId>("contract-client"),
            "Ship the Windows contract matrix",
            "in_progress",
            std::optional<std::string>{"forge-conductor"},
            std::optional<std::string>{"D:/workspace"},
            {},
            {"Continue current work"},
            {"D:/workspace/src/main.cpp"},
            {"Keep the matrix deterministic"},
            {},
            "Deterministic continuity record",
            "Resume the deterministic contract run.",
            false},
        1U,
        {}};
}

class MatrixLegacyContinuity final
    : public Contracts::ILegacyContextContinuityService {
public:
    MatrixLegacyContinuity(EffectLedger& effects, const DependencyMode mode)
        : effects_{effects}, mode_{mode}
    {
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        const Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.checkpoint");
        return persistResult(false);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        const Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.handoff");
        return persistResult(true);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest&,
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.automatic_persist");
        return persistResult(false);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId&,
        const std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.budget_handoff");
        return persistResult(true);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.get");
        return resultFor(
            mode_,
            Domain::LegacyContinuityGetOutcome{
                legacyContinuityRecord(), request.handoffId.has_value()});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.list");
        const auto record = legacyContinuityRecord();
        return resultFor(
            mode_,
            Domain::LegacyContinuityListOutcome{
                {Domain::LegacyContinuityListItem{
                    record.packet.id,
                    record.packet.updatedAt,
                    record.packet.source,
                    record.packet.resumeReady,
                    record.packet.goal,
                    record.packet.status,
                    record.packet.agents.size(),
                    record.writeSequence}}});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityStatusSummary>
    statusSummary(const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.status_summary");
        Domain::LegacyContinuityStatusSummary status;
        status.latestId = parse<Domain::LegacyHandoffId>("contract-handoff");
        status.resumeReady = true;
        status.resumeId = status.latestId;
        return resultFor(mode_, std::move(status));
    }

    [[nodiscard]] Domain::Result<
        Domain::LegacyContinuityProjectionRepairOutcome> repairProjections(
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.repair_projections");
        return resultFor(
            mode_, Domain::LegacyContinuityProjectionRepairOutcome{});
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        effects_.record("legacy_continuity.reset");
        return resultFor(mode_, Domain::LegacyContinuityResetOutcome{});
    }

    void shutdown() noexcept override {}

private:
    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    persistResult(const bool handoffRequired) const
    {
        return resultFor(
            mode_,
            Domain::LegacyContinuityPersistOutcome{
                legacyContinuityRecord(),
                handoffRequired,
                true,
                false,
                std::nullopt,
                {}});
    }

    EffectLedger& effects_;
    DependencyMode mode_;
};

template <typename T>
void seed(
    Fakes::DeterministicResult<T>& target,
    const DependencyMode mode,
    T value)
{
    target.set(resultFor(mode, std::move(value)));
}

void seedVoid(
    Fakes::DeterministicResult<void>& target,
    const DependencyMode mode)
{
    target.set(voidResultFor(mode));
}

[[nodiscard]] Domain::AgentSpec agentSpec()
{
    return Domain::AgentSpec{
        parse<Domain::AgentId>("explore"),
        "Explore",
        "Inspect the authorized workspace",
        {"fs_read", "search_text"},
        {"shell_exec"},
        {"Repository exploration"},
        {"Inspect the relevant files"},
        {"Report deterministic findings"},
        {"summary"},
        {},
        {},
        "Inspect the workspace and report concise findings.",
        "builtin"};
}

[[nodiscard]] Domain::AgentRunRecord agentRun(
    const Domain::ProjectId& projectId,
    const Domain::ClientId& clientId,
    const Domain::PathText& root)
{
    return Domain::AgentRunRecord{
        Domain::AgentSession{
            parse<Domain::SessionId>(
                "80000000-0000-4000-8000-000000000008"),
            parse<Domain::AgentId>("explore"),
            clientId,
            Domain::SessionStatus::Open,
            std::nullopt,
            Domain::UtcTimePoint{},
            Domain::UtcTimePoint{}},
        projectId,
        std::optional<std::string>{"Inspect the workspace"},
        root,
        {"summary"},
        {"Inspect the relevant files"},
        std::nullopt};
}

[[nodiscard]] Domain::ProcessResult processResultFor(
    const DependencyMode mode)
{
    return Domain::ProcessResult{
        mode == DependencyMode::ProcessNonzero ? 7 : 0,
        mode == DependencyMode::ProcessNonzero
            ? "partial contract output\n"
            : "contract output\n",
        mode == DependencyMode::ProcessNonzero ? "contract failure\n" : "",
        false,
        false,
        false,
        false,
        true,
        1ms};
}

[[nodiscard]] Domain::ProjectMemoryRecord memoryRecord(
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& recordId,
    const Domain::Sha256Digest& digest)
{
    return Domain::ProjectMemoryRecord{
        recordId,
        projectId,
        1U,
        "fact",
        "Contract record",
        "A deterministic project-memory record",
        std::optional<std::string>{"Contract body"},
        {"contract"},
        0.5,
        1.0,
        "external_integration",
        std::nullopt,
        std::nullopt,
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{},
        std::nullopt,
        digest,
        false,
        Domain::ProjectMemorySchemaVersion};
}

[[nodiscard]] Domain::ContinuityOperation continuityOperation(
    const Domain::ProjectId& projectId,
    const Domain::Sha256Digest& digest)
{
    return Domain::ContinuityOperation{
        parse<Domain::ContinuityOperationId>(
            "20000000-0000-4000-8000-000000000002"),
        projectId,
        parse<Domain::SessionId>(
            "40000000-0000-4000-8000-000000000004"),
        parse<Domain::SessionId>(
            "50000000-0000-4000-8000-000000000005"),
        parse<Domain::ContinuityHandoffId>(
            "30000000-0000-4000-8000-000000000003"),
        Domain::ContinuityState::Acknowledged,
        1U,
        parse<Domain::AdapterId>("external-mcp"),
        take(Domain::IdempotencyKey::create("contract-continuity")),
        parse<Domain::SessionId>(
            "50000000-0000-4000-8000-000000000005"),
        parse<Domain::ContinuityHandoffId>(
            "30000000-0000-4000-8000-000000000003"),
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{},
        std::nullopt,
        std::nullopt,
        digest,
        std::nullopt};
}

[[nodiscard]] Domain::ContinuityHandoff continuityHandoff(
    const Domain::ProjectMemoryDescriptor& descriptor,
    const Domain::ContinuityOperation& operation,
    const Domain::Sha256Digest& digest)
{
    return Domain::ContinuityHandoff{
        operation.handoffId,
        operation.operationId,
        Domain::UtcTimePoint{},
        Domain::ContinuityProject{
            descriptor.id,
            descriptor.displayName,
            descriptor.aliases.front(),
            "main",
            "abc1234",
            {}},
        Domain::ContinuitySession{
            operation.predecessorSessionId,
            std::nullopt,
            std::nullopt,
            std::optional<std::string>{"contract"}},
        Domain::ContinuitySession{
            operation.successorSessionId.value(),
            std::nullopt,
            std::nullopt,
            std::optional<std::string>{"contract"}},
        "Ship the Windows contract matrix",
        {"Preserve parity"},
        Domain::ContinuityCurrentWork{
            "P14",
            "mcp-contract-matrix",
            "Exercise every MCP tool",
            {}},
        {},
        {Domain::ContinuityWorkEntry{
            std::nullopt, "Run the next contract case", "open"}},
        {Domain::ContinuityDecision{
            "Use an in-process router", std::nullopt}},
        Domain::ContinuityValidation{{"T-MCP"}, {}, {}},
        {},
        {},
        {Domain::ContinuityNextAction{
            1U,
            "Continue current work",
            "",
            "The matrix remains deterministic"}},
        Domain::ContinuityHostState{
            operation.adapterId,
            Domain::ContinuityState::CheckpointPersisted,
            "caller_reported",
            Domain::ContinuityRetryState{}},
        digest,
        true};
}

[[nodiscard]] Domain::HostSession hostSession(
    const Domain::ProjectId& projectId,
    const Domain::ContinuityOperation& operation)
{
    return Domain::HostSession{
        operation.successorSessionId.value(),
        projectId,
        operation.operationId,
        operation.predecessorSessionId,
        operation.idempotencyKey,
        std::nullopt,
        std::nullopt,
        Domain::HostSessionStatus::Ready};
}

} // namespace

class McpToolContractFixture::Impl final {
public:
    explicit Impl(const DependencyMode mode)
        : mode_{mode},
          projectId_{parse<Domain::ProjectId>(
              "10000000-0000-4000-8000-000000000001")},
          clientId_{parse<Domain::ClientId>("contract-client")},
          root_{take(Domain::PathText::create("D:/workspace"))},
          digest_{parse<Domain::Sha256Digest>(
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")},
          descriptor_{
              projectId_,
              "Forge Conductor contract",
              std::optional<std::string>{"forge-conductor-contract"},
              {root_}},
          clock_{
              Domain::UtcTimePoint{1'735'789'855s},
              Domain::MonotonicTimePoint{1s}},
          hasher_{digest_},
          uuidGenerator_{uuidValues()},
          catalog_{take(Mcp::McpToolCatalog::create())},
          reportInspector_{effects_, mode_},
          legacyContinuity_{effects_, mode_},
          clientWorkspace_{
              effects_,
              mode_,
              Domain::ClientWorkspaceAdoption{
                  Domain::ClientWorkspaceSnapshot{
                      clientId_,
                      projectId_,
                      root_,
                      parse<Domain::LegacyHandoffId>("contract-handoff"),
                      1U,
                      2U},
                  std::nullopt,
                  false}},
          workspaceAuthority_{
              parse<Domain::AuthorityId>(
                  "90000000-0000-4000-8000-000000000009"),
              clientId_,
              {root_},
              Domain::FileAccess::Write,
              {Domain::FileAccess::Read,
               Domain::FileAccess::Write,
               Domain::FileAccess::Create,
               Domain::FileAccess::Delete,
               Domain::FileAccess::Execute},
              {},
              true,
              1U},
          authority_{take(workspaceAuthority_.authorityFor(
              projectId_, operationContext()))},
          fileSystem_{3U * 1024U * 1024U},
          fileTextServices_{effects_, mode_},
          git_{200U},
          legacyMemory_{effects_, mode_},
          projectRegistry_{effects_, mode_, descriptor_},
          continuityCodec_{
              effects_,
              mode_,
              continuityHandoff(
                  descriptor_,
                  continuityOperation(projectId_, digest_),
                  digest_)},
          continuityAutomation_{effects_},
          forgeStatus_{effects_, mode_},
          authorizer_{clock_},
          audit_{8U, clock_.monotonicNow()}
    {
        seedDependencies();
        const auto shellExecutable = take(Domain::PathText::create(
            "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));
        adapter_ = take(Mcp::McpToolPackAdapter::create(
            Mcp::McpToolPackDependencies{
                *catalog_,
                applicationPaths_,
                agentCatalog_,
                agentSessions_,
                reportInspector_,
                legacyContinuity_,
                clientWorkspace_,
                workspaceAuthority_,
                fileSystem_,
                fileTextServices_,
                fileTextServices_,
                git_,
                legacyMemory_,
                pdf_,
                textSearch_,
                shell_,
                projectRegistry_,
                projectMemory_,
                continuity_,
                continuityCodec_,
                continuityAutomation_,
                forgeStatus_,
                clock_,
                uuidGenerator_,
                projectMemoryLimits_,
                std::chrono::seconds{30},
                shellExecutable,
                "0.9.0",
                "windows-cpp",
                77U}));
        std::array<Contracts::IToolHandler*, 1U> handlers{adapter_.get()};
        router_ = take(Mcp::McpToolRouter::create(
            *catalog_,
            handlers,
            authorizer_,
            invocationGuard_,
            audit_,
            hasher_,
            clock_));
    }

    [[nodiscard]] InvocationResult invoke(
        const std::string_view toolName,
        const Json& arguments)
    {
        prepareInvocation(toolName, arguments);
        const auto context = operationContext();
        const Domain::ToolCallRequest request{
            Domain::McpRequestMetadata{
                parse<Domain::RequestId>("contract-request"),
                context.correlationId,
                clientId_,
                projectId_,
                "2025-06-18"},
            std::string{toolName},
            arguments.dump()};
        auto invoked = router_->invoke(request, authority_, context);
        InvocationResult result;
        if (!invoked) {
            result.errorCode = invoked.error().code;
            result.errorRetryable = invoked.error().retryable;
        } else {
            result.hasOutcome = true;
            result.receiptOk = invoked.value().receipt.ok;
            if (invoked.value().receipt.error) {
                result.receiptErrorCode =
                    invoked.value().receipt.error->code;
            }
            result.payload = Json::parse(invoked.value().canonicalPayload);
        }
        collectRecordedEffects();
        result.observations = observations();
        result.effects = effects_.sorted();
        return result;
    }

    [[nodiscard]] std::vector<std::string> catalogToolNames() const
    {
        std::vector<std::string> names;
        names.reserve(catalog_->tools().size());
        for (const auto& descriptor : catalog_->tools()) {
            names.push_back(descriptor.tool.name);
        }
        return names;
    }

private:
    [[nodiscard]] static std::vector<Domain::Uuid> uuidValues()
    {
        return {
            take(Domain::Uuid::parse(
                "21000000-0000-4000-8000-000000000012")),
            take(Domain::Uuid::parse(
                "31000000-0000-4000-8000-000000000013")),
            take(Domain::Uuid::parse(
                "22000000-0000-4000-8000-000000000022")),
            take(Domain::Uuid::parse(
                "32000000-0000-4000-8000-000000000023"))};
    }

    [[nodiscard]] Domain::OperationContext operationContext() const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>(
                "a0000000-0000-4000-8000-00000000000a"),
            clock_.monotonicNow() + 5min,
            {},
            parse<Domain::CorrelationId>("contract-correlation")};
    }

    void seedDependencies()
    {
        seed(applicationPaths_.dataRootResult, mode_, root_);
        seed(applicationPaths_.configurationRootResult, mode_, root_);
        seed(applicationPaths_.diagnosticsRootResult, mode_, root_);
        seed(applicationPaths_.projectRootResult, mode_, root_);

        const auto spec = agentSpec();
        seed(
            agentCatalog_.allResult,
            mode_,
            std::vector<Domain::AgentSpec>{spec});
        seed(
            agentCatalog_.getResult,
            mode_,
            std::optional<Domain::AgentSpec>{spec});
        seed(agentCatalog_.recommendResult, mode_, spec);

        auto run = agentRun(projectId_, clientId_, root_);
        seed(agentSessions_.startResult, mode_, run.session);
        seed(agentSessions_.statusResult, mode_, run.session);
        seed(agentSessions_.completeResult, mode_, run.session);
        seed(agentSessions_.pruneStaleResult, mode_, std::size_t{0U});
        seed(
            agentSessions_.startRunResult,
            mode_,
            Domain::AgentRunStartOutcome{
                run, std::nullopt, spec, 0U, true});
        seed(
            agentSessions_.runStatusResult,
            mode_,
            Domain::AgentRunStatusOutcome{
                run, true, 1, false, false, std::nullopt});
        auto completedRun = run;
        completedRun.session.status = Domain::SessionStatus::Completed;
        completedRun.reportJson = "{\"summary\":\"done\"}";
        seed(
            agentSessions_.completeRunResult,
            mode_,
            Domain::AgentRunCompleteOutcome{
                completedRun,
                Domain::AgentCompletionReport{
                    "{\"summary\":\"done\"}",
                    {{"summary", Domain::AgentReportValueKind::String, 4U}}},
                true,
                {}});
        seed(
            agentSessions_.attachResult,
            mode_,
            Domain::AgentRunReattachOutcome{
                run,
                Domain::ActiveBinding{
                    run.session.id,
                    run.session.agentId,
                    run.goal.value_or(""),
                    spec.tools,
                    spec.toolsForbidden,
                    run.outputSchema,
                    spec.doneDefinition,
                    run.workingDirectory},
                std::nullopt,
                0U,
                false});
        seed(
            agentSessions_.rehydrateResult,
            mode_,
            Domain::AgentRunRecoveryOutcome{});
        seed(
            agentSessions_.bindingResult,
            mode_,
            std::optional<Domain::ActiveBinding>{});
        seed(agentSessions_.touchIfActiveResult, mode_, false);

        std::vector<std::byte> readBytes(
            120U * 1024U,
            static_cast<std::byte>(static_cast<unsigned char>('x')));
        seed(fileSystem_.readFileResult, mode_, std::move(readBytes));
        seedVoid(fileSystem_.writeFileResult, mode_);
        seed(
            fileSystem_.listResult,
            mode_,
            std::vector<Domain::PathText>{take(
                Domain::PathText::create("D:/workspace/src"))});
        seedVoid(fileSystem_.createDirectoryResult, mode_);
        seedVoid(fileSystem_.removeResult, mode_);
        seedVoid(fileSystem_.moveResult, mode_);

        const auto process = processResultFor(mode_);
        seed(git_.statusResult, mode_, process);
        seed(git_.diffResult, mode_, process);
        seed(git_.logResult, mode_, process);
        seed(git_.addResult, mode_, process);
        seed(git_.commitResult, mode_, process);

        const auto pdfPath = take(
            Domain::PathText::create("D:/workspace/output.pdf"));
        seed(
            pdf_.writeResult,
            mode_,
            Domain::PdfWriteReceipt{
                pdfPath, 128U, 1U, "contract", "Contract PDF"});
        seed(
            pdf_.fromTextFileResult,
            mode_,
            Domain::PdfWriteReceipt{
                pdfPath, 128U, 1U, "contract", "Contract PDF"});
        seed(
            textSearch_.searchResult,
            mode_,
            std::vector<std::string>{"src/main.cpp:1:contract"});
        seed(shell_.executeResult, mode_, process);

        const auto recordId = parse<Domain::MemoryRecordId>(
            "60000000-0000-4000-8000-000000000006");
        const auto record = memoryRecord(projectId_, recordId, digest_);
        const Domain::MemoryWriteOutcome writeOutcome{
            projectId_,
            recordId,
            1U,
            Domain::MemoryWriteDisposition::Inserted,
            digest_};
        seed(
            projectMemory_.initializeResult,
            mode_,
            Domain::ProjectInitialization{
                descriptor_,
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion,
                projectMemoryLimits_,
                true,
                false,
                true});
        seed(projectMemory_.rememberResult, mode_, writeOutcome);
        seed(
            projectMemory_.rememberBatchResult,
            mode_,
            Domain::MemoryBatchOutcome{projectId_, {writeOutcome}});
        const Domain::MemoryPage page{
            projectId_,
            {{record, 1.0}},
            std::nullopt,
            false,
            512U,
            projectMemoryLimits_.defaultResponseBytes};
        seed(projectMemory_.searchResult, mode_, page);
        seed(
            projectMemory_.getResult,
            mode_,
            Domain::MemoryRecords{
                projectId_,
                {record},
                512U,
                projectMemoryLimits_.defaultResponseBytes});
        seed(projectMemory_.updateResult, mode_, record);
        seed(
            projectMemory_.forgetResult,
            mode_,
            Domain::ForgetOutcome{
                projectId_, recordId, Domain::ForgetDisposition::Tombstoned});
        seed(projectMemory_.listRecentResult, mode_, page);
        seed(
            projectMemory_.linkResult,
            mode_,
            Domain::LinkOutcome{
                projectId_, Domain::LinkDisposition::Inserted});
        seed(
            projectMemory_.exportResult,
            mode_,
            Domain::ProjectMemoryExport{
                projectId_,
                take(Domain::PathText::create(
                    "D:/workspace/project-memory-export.json")),
                digest_,
                1U});
        seed(
            projectMemory_.importResult,
            mode_,
            Domain::ProjectMemoryImport{
                projectId_,
                Domain::ImportDisposition::Preview,
                1U,
                1U,
                digest_,
                {}});
        seed(
            projectMemory_.statusResult,
            mode_,
            Domain::ProjectMemoryStatus{
                projectId_,
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion,
                1U,
                0U,
                1U,
                4096U,
                0U,
                false,
                true,
                1U,
                projectMemoryLimits_});
        seedVoid(projectMemory_.closeProjectResult, mode_);
        seed(
            projectMemory_.resetProjectMemoryResult,
            mode_,
            Domain::ResetReport{});
        seed(
            projectMemory_.resetAllProjectMemoryResult,
            mode_,
            Domain::ResetReport{});

        const auto operation = continuityOperation(projectId_, digest_);
        const auto handoff = continuityHandoff(
            descriptor_, operation, digest_);
        const Domain::CheckpointOutcome checkpoint{operation, handoff};
        seed(continuity_.checkpointResult, mode_, checkpoint);
        seed(continuity_.prepareHandoffResult, mode_, checkpoint);
        seed(
            continuity_.pendingHandoffResult,
            mode_,
            std::optional<Domain::ContinuityHandoff>{handoff});
        seed(continuity_.acknowledgeHandoffResult, mode_, operation);
        const auto session = hostSession(projectId_, operation);
        seed(
            continuity_.resumeResult,
            mode_,
            Domain::HandoffResumeOutcome{operation, handoff, session});
        seed(
            continuity_.statusResult,
            mode_,
            Domain::ContinuityStatus{
                projectId_, operation, 1U, 1U, false});
        seed(
            continuity_.rolloverResult,
            mode_,
            Domain::RolloverOutcome{
                operation, session, true, true});
        seed(
            continuity_.recoveryResult,
            mode_,
            Domain::ContinuityRecoveryReport{});
        seed(
            continuity_.resetResult,
            mode_,
            Domain::ContinuityResetReport{projectId_, Domain::ResetReport{}});
    }

    void prepareInvocation(
        const std::string_view toolName,
        const Json& arguments)
    {
        if (toolName == "continuity.status" &&
            mode_ != DependencyMode::DatabaseBusy && arguments.is_object()) {
            const auto rawProjectId = arguments.find("project_id");
            if (rawProjectId != arguments.end() && rawProjectId->is_string() &&
                rawProjectId->get<std::string>() != projectId_.value()) {
                seed(
                    continuity_.statusResult,
                    mode_,
                    Domain::ContinuityStatus{
                        projectId_, std::nullopt, 0U, 0U, false});
            }
        }
        if (toolName != "project_memory.import" ||
            mode_ == DependencyMode::DatabaseBusy ||
            !arguments.is_object() ||
            arguments.value("preview", true)) {
            return;
        }
        const auto recordId = parse<Domain::MemoryRecordId>(
            "60000000-0000-4000-8000-000000000006");
        const Domain::MemoryWriteOutcome imported{
            projectId_,
            recordId,
            1U,
            Domain::MemoryWriteDisposition::Inserted,
            digest_};
        seed(
            projectMemory_.importResult,
            mode_,
            Domain::ProjectMemoryImport{
                projectId_,
                Domain::ImportDisposition::Imported,
                1U,
                1U,
                digest_,
                {imported}});
    }

    void collectRecordedEffects()
    {
        if (applicationPaths_.calls() != 0U) {
            effects_.record("application_paths.data_root");
        }
        if (agentCatalog_.lastCapture()) {
            switch (agentCatalog_.lastCapture()->call) {
            case Fakes::AgentCatalogCall::All:
                effects_.record("agent_catalog.all");
                break;
            case Fakes::AgentCatalogCall::Get:
                effects_.record("agent_catalog.get");
                break;
            case Fakes::AgentCatalogCall::Recommend:
                effects_.record("agent_catalog.recommend");
                break;
            }
        }
        if (agentSessions_.lastCapture()) {
            switch (agentSessions_.lastCapture()->call) {
            case Fakes::AgentSessionServiceCall::StartRun:
                effects_.record("agent_sessions.start_run");
                break;
            case Fakes::AgentSessionServiceCall::RunStatus:
                effects_.record("agent_sessions.run_status");
                break;
            case Fakes::AgentSessionServiceCall::CompleteRun:
                effects_.record("agent_sessions.complete_run");
                break;
            default:
                break;
            }
        }
        if (fileSystem_.lastCapture()) {
            switch (fileSystem_.lastCapture()->call) {
            case Fakes::FileSystemCall::ReadFile:
                effects_.record("file_system.read_file");
                break;
            case Fakes::FileSystemCall::WriteFile:
                effects_.record("file_system.write_file");
                break;
            case Fakes::FileSystemCall::List:
                effects_.record("file_system.list");
                break;
            case Fakes::FileSystemCall::CreateDirectory:
                effects_.record("file_system.create_directory");
                break;
            case Fakes::FileSystemCall::Remove:
                effects_.record("file_system.remove");
                break;
            case Fakes::FileSystemCall::Move:
                effects_.record("file_system.move");
                break;
            }
        }
        if (git_.lastCapture()) {
            switch (git_.lastCapture()->call) {
            case Fakes::GitServiceCall::Status:
                effects_.record("git.status");
                break;
            case Fakes::GitServiceCall::Diff:
                effects_.record("git.diff");
                break;
            case Fakes::GitServiceCall::Log:
                effects_.record("git.log");
                break;
            case Fakes::GitServiceCall::Add:
                effects_.record("git.add");
                break;
            case Fakes::GitServiceCall::Commit:
                effects_.record("git.commit");
                break;
            }
        }
        if (pdf_.lastCapture()) {
            effects_.record(
                pdf_.lastCapture()->call == Fakes::PdfServiceCall::Write
                    ? "pdf.write"
                    : "pdf.from_text_file");
        }
        if (textSearch_.lastCapture()) {
            effects_.record("text_search.search");
        }
        if (shell_.lastExecution()) {
            effects_.record("shell.execute");
        }
        collectProjectMemoryEffects();
        collectContinuityEffects();
    }

    void collectProjectMemoryEffects()
    {
        constexpr std::array<std::pair<Fakes::ProjectMemoryCall, const char*>,
                             12U>
            mappings{{
                {Fakes::ProjectMemoryCall::Initialize,
                 "project_memory.initialize"},
                {Fakes::ProjectMemoryCall::Remember,
                 "project_memory.remember"},
                {Fakes::ProjectMemoryCall::RememberBatch,
                 "project_memory.remember_batch"},
                {Fakes::ProjectMemoryCall::Search,
                 "project_memory.search"},
                {Fakes::ProjectMemoryCall::Get, "project_memory.get"},
                {Fakes::ProjectMemoryCall::Update,
                 "project_memory.update"},
                {Fakes::ProjectMemoryCall::Forget,
                 "project_memory.forget"},
                {Fakes::ProjectMemoryCall::ListRecent,
                 "project_memory.list_recent"},
                {Fakes::ProjectMemoryCall::Link, "project_memory.link"},
                {Fakes::ProjectMemoryCall::Export,
                 "project_memory.export"},
                {Fakes::ProjectMemoryCall::Import,
                 "project_memory.import"},
                {Fakes::ProjectMemoryCall::Status,
                 "project_memory.status"}}};
        for (const auto& [call, name] : mappings) {
            if (projectMemory_.callCount(call) != 0U) {
                effects_.record(name);
            }
        }
    }

    void collectContinuityEffects()
    {
        constexpr std::array<std::pair<Fakes::ContinuityCall, const char*>, 7U>
            mappings{{
                {Fakes::ContinuityCall::Checkpoint,
                 "continuity.checkpoint"},
                {Fakes::ContinuityCall::PrepareHandoff,
                 "continuity.prepare_handoff"},
                {Fakes::ContinuityCall::GetPendingHandoff,
                 "continuity.get_pending_handoff"},
                {Fakes::ContinuityCall::AcknowledgeHandoff,
                 "continuity.acknowledge_handoff"},
                {Fakes::ContinuityCall::Resume, "continuity.resume"},
                {Fakes::ContinuityCall::Status, "continuity.status"},
                {Fakes::ContinuityCall::RequestRollover,
                 "continuity.request_rollover"}}};
        for (const auto& [call, name] : mappings) {
            if (continuity_.callCount(call) != 0U) {
                effects_.record(name);
            }
        }
    }

    [[nodiscard]] Json observations() const
    {
        Json result = Json::object();
        if (git_.lastCapture() &&
            git_.lastCapture()->call == Fakes::GitServiceCall::Commit) {
            result["git_commit_message"] = git_.lastCapture()->message;
        }
        if (shell_.lastExecution() && shell_.lastExecution()->request) {
            result["shell_timeout_ms"] =
                shell_.lastExecution()->request->timeout.count();
        }
        if (pdf_.lastCapture() &&
            pdf_.lastCapture()->call == Fakes::PdfServiceCall::FromTextFile &&
            pdf_.lastCapture()->secondary) {
            result["pdf_destination"] =
                pdf_.lastCapture()->secondary->canonicalPath().value();
        }
        if (fileSystem_.lastCapture() &&
            fileSystem_.lastCapture()->call == Fakes::FileSystemCall::Move &&
            fileSystem_.lastCapture()->secondary) {
            result["fs_move_source"] =
                fileSystem_.lastCapture()->primary.canonicalPath().value();
            result["fs_move_destination"] = fileSystem_.lastCapture()
                ->secondary->canonicalPath().value();
        }
        if (projectMemory_.lastProjectId()) {
            result["project_memory_project_id"] =
                projectMemory_.lastProjectId()->value();
        }
        return result;
    }

    DependencyMode mode_;
    EffectLedger effects_;
    Domain::ProjectId projectId_;
    Domain::ClientId clientId_;
    Domain::PathText root_;
    Domain::Sha256Digest digest_;
    Domain::ProjectMemoryDescriptor descriptor_;
    Domain::ProjectMemoryLimits projectMemoryLimits_;
    Fakes::FakeClock clock_;
    Fakes::ScriptedHasher hasher_;
    Fakes::SequenceUuidGenerator uuidGenerator_;
    std::unique_ptr<Mcp::McpToolCatalog> catalog_;
    Fakes::RecordingApplicationPathsFake applicationPaths_;
    Fakes::RecordingAgentCatalogFake agentCatalog_;
    Fakes::RecordingAgentSessionServiceFake agentSessions_;
    MatrixReportInspector reportInspector_;
    MatrixLegacyContinuity legacyContinuity_;
    MatrixClientWorkspaceContext clientWorkspace_;
    Fakes::DeterministicWorkspaceAuthority workspaceAuthority_;
    Contracts::WorkspaceAuthority authority_;
    Fakes::RecordingFileSystemFake fileSystem_;
    MatrixFileTextServices fileTextServices_;
    Fakes::RecordingGitServiceFake git_;
    MatrixLegacyMemory legacyMemory_;
    Fakes::RecordingPdfServiceFake pdf_;
    Fakes::RecordingTextSearchServiceFake textSearch_;
    Fakes::RecordingShellServiceFake shell_;
    MatrixProjectRegistry projectRegistry_;
    Fakes::RecordingProjectMemoryService projectMemory_;
    Fakes::RecordingContinuityCoordinator continuity_;
    MatrixContinuityCodec continuityCodec_;
    MatrixContinuityAutomationStatus continuityAutomation_;
    MatrixForgeStatusRepository forgeStatus_;
    PassThroughInvocationGuard invocationGuard_;
    Mcp::McpToolAuthorizer authorizer_;
    Fakes::AuditRepositoryFake audit_;
    std::unique_ptr<Mcp::McpToolPackAdapter> adapter_;
    std::unique_ptr<Mcp::McpToolRouter> router_;
};

McpToolContractFixture::McpToolContractFixture(const DependencyMode mode)
    : implementation_{std::make_unique<Impl>(mode)}
{
}

McpToolContractFixture::~McpToolContractFixture() noexcept = default;

InvocationResult McpToolContractFixture::invoke(
    const std::string_view toolName,
    const Json& arguments)
{
    return implementation_->invoke(toolName, arguments);
}

std::vector<std::string> McpToolContractFixture::catalogToolNames() const
{
    return implementation_->catalogToolNames();
}

} // namespace ForgeConductor::Tests::McpContract
