#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpToolPackAdapter.h"
#include "Fakes/ApplicationServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FileSystemFake.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/GitServiceFake.h"
#include "Fakes/PdfServiceFake.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/RecordingContinuityCoordinator.h"
#include "Fakes/RecordingProjectMemoryService.h"
#include "Fakes/ShellServiceFake.h"
#include "Fakes/TelemetryFakes.h"
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
        const Domain::LegacyContinuityAutomaticRequest&,
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
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
        return unavailable<Domain::LegacyContinuityGetOutcome>(message_);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::LegacyContinuityListOutcome>(message_);
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

private:
    static constexpr const char* message_ =
        "Legacy continuity is not configured for this test.";
};

class PassiveFileTextServices final
    : public Contracts::ITextFileEditService,
      public Contracts::IPathGlobService {
public:
    [[nodiscard]] Domain::Result<Contracts::TextFileEditReport> replaceAll(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        std::string_view,
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
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
    : public Contracts::IContinuityAutomation {
public:
    [[nodiscard]] Domain::Result<Domain::ContinuityAutomationOutcome> observe(
        const Domain::ContinuityAutomationObservation&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ContinuityAutomationOutcome>(
            "Continuity automation is not configured for this test.");
    }

    void cancel(const Domain::OperationId&) noexcept override {}
    [[nodiscard]] std::size_t trackedProjectCount() const noexcept override
    {
        return 0U;
    }
    void shutdown() noexcept override {}
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
    Fakes::RecordingAgentSessionServiceFake agentSessions;
    PassiveReportInspector reportInspector;
    PassiveLegacyContinuity legacyContinuity;
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
    Fakes::RecordingTelemetryService telemetry;
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
            telemetry,
            clock,
            uuidGenerator,
            Domain::ProjectMemoryLimits{},
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

} // namespace

int main()
{
    try {
        testHandlerContract();
        testAllCatalogPacksAreBoundedByTheAdapterContract();
        testRuntimeDispatchAndSchemaPolicy();
        std::cout << "MCP tool-pack adapter tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP tool-pack adapter tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
