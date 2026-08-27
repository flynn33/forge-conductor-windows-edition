#include "TestSupport.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsLMStudioServeVerifier;
using Json = nlohmann::json;
using namespace std::chrono_literals;

static_assert(std::is_final_v<WindowsLMStudioServeVerifier>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationContext operationContext(const std::uint32_t index)
{
    std::array<char, 37U> operation{};
    const auto written = std::snprintf(operation.data(), operation.size(),
                                       "%08x-0000-4000-8000-%012x", index, index);
    require(written == 36, "the verifier operation UUID could not be formatted");
    return Domain::OperationContext{
        parse<Domain::OperationId>(operation.data()),
        std::chrono::steady_clock::now() + 5min,
        {},
        parse<Domain::CorrelationId>("p15-lmstudio-serve-verifier")};
}

class AuthorityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::WorkspaceAuthority create()
    {
        return take(issueAuthority(
            parse<Domain::AuthorityId>("15151515-1515-4515-8515-151515151515"),
            parse<Domain::ProjectId>("25252525-2525-4525-8525-252525252525"),
            parse<Domain::ClientId>("p15-lmstudio-verifier"),
            {path("C:\\Forge")},
            Domain::FileAccess::Write,
            {Domain::FileAccess::Read, Domain::FileAccess::Write,
             Domain::FileAccess::Execute},
            {},
            true,
            15U));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&, const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The verifier test issuer only exposes its deterministic factory."));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The verifier test issuer does not narrow authorities."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The verifier test issuer does not authorize paths."));
    }
};

[[nodiscard]] Json canonicalTools()
{
    const auto fixture = std::filesystem::path{__FILE__}.parent_path().parent_path() /
        L"fixtures" / L"Mcp" / L"mcp-tools-semantic-golden.json";
    std::ifstream input{fixture, std::ios::binary};
    require(input.good(), "the reviewed MCP semantic golden could not be opened");
    const std::string encoded{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    require(!encoded.empty() && encoded.size() <= 80'000U,
            "the reviewed MCP semantic golden is empty or unbounded");
    const auto document = Json::parse(encoded);
    require(document.is_object() && document.value("schemaVersion", 0) == 1 &&
                document.contains("tools") && document.at("tools").is_array() &&
                document.at("tools").size() == 53U,
            "the reviewed MCP semantic golden has the wrong schema or tool count");
    return document.at("tools");
}

[[nodiscard]] Json projectMemoryCapability()
{
    const Domain::ProjectMemoryLimits limits;
    return Json{
        {"capabilityVersion", Domain::ProjectMemoryCapabilityVersion},
        {"limits",
         Json{
             {"batch_bytes", limits.maximumBatchBytes},
             {"batch_count", limits.maximumBatchCount},
             {"body_bytes", limits.maximumBodyBytes},
             {"open_projects", limits.maximumOpenProjects},
             {"page_count", limits.maximumPageCount},
             {"response_bytes", limits.maximumResponseBytes},
             {"source_reference_bytes", limits.maximumSourceReferenceBytes},
             {"summary_bytes", limits.maximumSummaryBytes},
             {"tag_bytes", limits.maximumTagBytes},
             {"tag_count", limits.maximumTagCount},
             {"title_bytes", limits.maximumTitleBytes}}},
        {"schemaVersion", Domain::ProjectMemorySchemaVersion}};
}

[[nodiscard]] std::string successfulResponse(
    const Domain::LMStudioConnectorRole role,
    const std::size_t toolCount = 53U,
    const bool duplicateLast = false,
    const bool reverseLastTwo = false,
    const bool emptyDescription = false,
    const bool wrongSchemaType = false,
    const bool wrongCanonicalName = false,
    const bool compatibleSchemaDrift = false)
{
    const Json initialize{
        {"id", 1},
        {"jsonrpc", "2.0"},
        {"result",
         Json{{"capabilities",
               Json{{"projectMemory", projectMemoryCapability()},
                    {"tools", Json{{"listChanged", false}}}}},
              {"protocolVersion", "2025-11-25"},
              {"serverInfo",
               Json{{"name", role == Domain::LMStudioConnectorRole::Fallback
                                  ? "forge-conductor-fallback"
                                  : "forge-conductor"},
                    {"version", "0.9.0"}}}}}};
    Json tools = canonicalTools();
    require(toolCount <= tools.size(), "the requested verifier tool subset is invalid");
    while (tools.size() > toolCount) {
        tools.erase(tools.end() - 1);
    }
    if (duplicateLast && tools.size() >= 2U) {
        tools.back()["name"] = tools[tools.size() - 2U].at("name");
    }
    if (emptyDescription && !tools.empty()) {
        tools.front()["description"] = "";
    }
    if (wrongSchemaType && !tools.empty()) {
        tools.front()["inputSchema"] = Json{{"type", "array"}};
    }
    if (wrongCanonicalName && !tools.empty()) {
        tools.front()["name"] = "agent_contexu";
    }
    if (compatibleSchemaDrift && !tools.empty()) {
        tools.front()["inputSchema"]["x-forge-test-drift"] = true;
    }
    if (reverseLastTwo && tools.size() >= 2U) {
        std::swap(tools[tools.size() - 1U], tools[tools.size() - 2U]);
    }
    const Json listed{{"id", 2},
                      {"jsonrpc", "2.0"},
                      {"result", Json{{"tools", std::move(tools)}}}};
    return initialize.dump() + '\n' + listed.dump() + '\n';
}

class ScriptedProcessSupervisor final : public Contracts::IProcessSupervisor {
public:
    void setOutput(std::string stdoutUtf8)
    {
        std::lock_guard lock{mutex_};
        outcome_ = {};
        outcome_.stdoutUtf8 = std::move(stdoutUtf8);
        failure_.reset();
        blocking_ = false;
        preAdmissionBlocking_ = false;
    }

    void setOutcome(Domain::ProcessResult outcome)
    {
        std::lock_guard lock{mutex_};
        outcome_ = std::move(outcome);
        failure_.reset();
        blocking_ = false;
        preAdmissionBlocking_ = false;
    }

    void setBlocking()
    {
        std::lock_guard lock{mutex_};
        blocking_ = true;
        ignoreCancellation_ = false;
        released_ = false;
        cancelled_ = false;
        activeOperation_.reset();
        failure_.reset();
    }

    void setBlockingUntilReleased()
    {
        std::lock_guard lock{mutex_};
        blocking_ = true;
        ignoreCancellation_ = true;
        released_ = false;
        cancelled_ = false;
        activeOperation_.reset();
        failure_.reset();
    }

    void setPreAdmissionBlocked()
    {
        std::lock_guard lock{mutex_};
        blocking_ = false;
        preAdmissionBlocking_ = true;
        preAdmissionEntered_ = false;
        preAdmissionCancellationObserved_ = false;
        allowPreAdmission_ = false;
        cancelled_ = false;
        activeOperation_.reset();
        failure_.reset();
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++calls_;
            lastRequest_ = request;
            lastAuthorityId_ = authority.authorityId().value();
            lastAuthorityIntent_ = authority.intent();
            lastContext_ = context;
            if (preAdmissionBlocking_) {
                preAdmissionEntered_ = true;
                stateChanged_.notify_all();
                stateChanged_.wait(lock, [&] {
                    return allowPreAdmission_ || context.cancellation.stop_requested() ||
                        processShutdown_;
                });
                if (context.cancellation.stop_requested() || processShutdown_) {
                    preAdmissionCancellationObserved_ =
                        context.cancellation.stop_requested();
                    stateChanged_.notify_all();
                    return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The scripted process was cancelled before admission."));
                }
            }
            if (blocking_) {
                activeOperation_ = context.operationId;
                stateChanged_.notify_all();
                stateChanged_.wait(lock, [&] {
                    return cancelled_ || processShutdown_ || released_;
                });
                activeOperation_.reset();
                if (released_ && !cancelled_ && !processShutdown_) {
                    return Domain::Result<Domain::ProcessResult>::success(outcome_);
                }
                return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The scripted process was cancelled."));
            }
            if (failure_) {
                return Domain::Result<Domain::ProcessResult>::failure(failure_.value());
            }
            return Domain::Result<Domain::ProcessResult>::success(outcome_);
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted process supervisor could not capture the request."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            cancelledOperations_.push_back(operationId.value());
            if (!ignoreCancellation_ && activeOperation_ &&
                activeOperation_.value() == operationId) {
                cancelled_ = true;
            }
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    void cancelAll() noexcept override
    {
        std::lock_guard lock{mutex_};
        cancelled_ = true;
        stateChanged_.notify_all();
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        processShutdown_ = true;
        stateChanged_.notify_all();
    }

    [[nodiscard]] bool waitUntilActive()
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(lock, 5s,
                                      [&] { return activeOperation_.has_value(); });
    }

    [[nodiscard]] bool waitUntilPreAdmission()
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(lock, 5s, [&] { return preAdmissionEntered_; });
    }

    [[nodiscard]] bool waitUntilPreAdmissionCancellation()
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(
            lock, 1s, [&] { return preAdmissionCancellationObserved_; });
    }

    [[nodiscard]] bool waitUntilCancellationCount(const std::size_t count)
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(
            lock, 5s, [&] { return cancelledOperations_.size() >= count; });
    }

    void releaseBlocking() noexcept
    {
        std::lock_guard lock{mutex_};
        released_ = true;
        stateChanged_.notify_all();
    }

    void releasePreAdmission() noexcept
    {
        std::lock_guard lock{mutex_};
        allowPreAdmission_ = true;
        stateChanged_.notify_all();
    }

    [[nodiscard]] std::optional<Domain::ProcessRequest> lastRequest() const
    {
        std::lock_guard lock{mutex_};
        return lastRequest_;
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::vector<std::string> cancelledOperations() const
    {
        std::lock_guard lock{mutex_};
        return cancelledOperations_;
    }

    [[nodiscard]] bool processShutdown() const noexcept
    {
        std::lock_guard lock{mutex_};
        return processShutdown_;
    }

    [[nodiscard]] Domain::FileAccess lastAuthorityIntent() const noexcept
    {
        std::lock_guard lock{mutex_};
        return lastAuthorityIntent_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    Domain::ProcessResult outcome_{};
    std::optional<Domain::Error> failure_;
    std::optional<Domain::ProcessRequest> lastRequest_;
    std::optional<Domain::OperationContext> lastContext_;
    std::optional<Domain::OperationId> activeOperation_;
    std::string lastAuthorityId_;
    Domain::FileAccess lastAuthorityIntent_{Domain::FileAccess::Read};
    std::vector<std::string> cancelledOperations_;
    std::size_t calls_{};
    bool blocking_{};
    bool ignoreCancellation_{};
    bool released_{};
    bool preAdmissionBlocking_{};
    bool preAdmissionEntered_{};
    bool preAdmissionCancellationObserved_{};
    bool allowPreAdmission_{};
    bool cancelled_{};
    bool processShutdown_{};
};

[[nodiscard]] std::optional<std::string> environmentValue(
    const Domain::ProcessRequest& request, const std::string_view name)
{
    const auto match = std::find_if(
        request.environment.begin(), request.environment.end(),
        [&](const Domain::EnvironmentVariable& variable) { return variable.name == name; });
    if (match == request.environment.end()) {
        return std::nullopt;
    }
    return match->value;
}

void testSuccessfulRoleVerificationAndRequestShape()
{
    ScriptedProcessSupervisor processes;
    processes.setOutput(successfulResponse(Domain::LMStudioConnectorRole::Primary));
    WindowsLMStudioServeVerifier verifier{processes};
    const auto authority = AuthorityIssuer::create();
    const auto deployment = parse<Domain::DeploymentId>("p15-deployment-001");
    const auto health = take(verifier.verify(
        path("C:\\Forge\\forge-conductor.exe"),
        path("C:\\Forge\\home"),
        Domain::LMStudioConnectorRole::Primary,
        deployment,
        authority,
        operationContext(1U)));

    require(health.role == Domain::LMStudioConnectorRole::Primary && health.ready,
            "the primary verifier did not return ready health");
    require(health.protocolVersion == "2025-11-25" && health.toolCount == 53U,
            "the primary verifier returned the wrong protocol or tool count");
    require(processes.lastAuthorityIntent() == Domain::FileAccess::Write,
            "the verifier rejected or rewrote a deploy authority with an Execute grant");

    const auto captured = processes.lastRequest();
    require(captured.has_value(), "the verifier did not issue a process request");
    require(captured->executable == path("C:\\Forge\\forge-conductor.exe"),
            "the verifier changed the selected binary path");
    require(captured->arguments == std::vector<std::string>{"serve"},
            "the verifier did not invoke the serve mode exactly");
    require(captured->workingDirectory == path("C:\\Forge\\home") &&
                captured->inheritEnvironment,
            "the verifier did not bind its inherited working directory to Forge home");
    require(captured->timeout == 15s && captured->maximumStdoutBytes == 80'000U &&
                captured->maximumStderrBytes == 20'000U,
            "the verifier process bounds were wrong");
    require(environmentValue(*captured, "FORGE_CONDUCTOR_HOME") == "C:\\Forge\\home" &&
                environmentValue(*captured, "FORGE_MCP_ROLE") == "primary" &&
                environmentValue(*captured, "FORGE_DEPLOYMENT_ID") ==
                    deployment.value(),
            "the verifier did not bind the primary environment exactly");
    require(captured->stdinUtf8.find("Content-Length:") == std::string::npos,
            "the verifier emitted Content-Length framing");
    require(std::count(captured->stdinUtf8.begin(), captured->stdinUtf8.end(), '\n') == 3,
            "the verifier did not emit the three-message NDJSON handshake");

    processes.setOutput(successfulResponse(Domain::LMStudioConnectorRole::Fallback));
    const auto fallback = take(verifier.verify(
        path("C:\\Forge\\forge-conductor.exe"),
        path("C:\\Forge\\home"),
        Domain::LMStudioConnectorRole::Fallback,
        std::nullopt,
        authority,
        operationContext(2U)));
    require(fallback.role == Domain::LMStudioConnectorRole::Fallback && fallback.ready &&
                fallback.toolCount == 53U,
            "the fallback verifier did not return ready health");
    const auto fallbackRequest = processes.lastRequest();
    require(fallbackRequest.has_value() &&
                environmentValue(*fallbackRequest, "FORGE_MCP_ROLE") == "fallback" &&
                environmentValue(*fallbackRequest, "FORGE_DEPLOYMENT_ID") == "",
            "the verifier did not suppress an ambient fallback deployment revision");
}

void testProtocolDriftAndProcessFailuresFailClosed()
{
    ScriptedProcessSupervisor processes;
    WindowsLMStudioServeVerifier verifier{processes};
    const auto authority = AuthorityIssuer::create();
    const auto verify = [&](const std::uint32_t index) {
        return verifier.verify(
            path("C:\\Forge\\forge-conductor.exe"),
            path("C:\\Forge\\home"),
            Domain::LMStudioConnectorRole::Primary,
            std::nullopt,
            authority,
            operationContext(index));
    };
    const auto mutateInitialize = [](std::string response, const auto& mutation) {
        const auto separator = response.find('\n');
        require(separator != std::string::npos,
                "the verifier response fixture has no initialize separator");
        auto initialize = Json::parse(response.substr(0U, separator));
        mutation(initialize);
        return initialize.dump() + response.substr(separator);
    };

    processes.setOutput(
        "Content-Length: 2\r\n\r\n{}\n" +
        successfulResponse(Domain::LMStudioConnectorRole::Primary));
    requireError(verify(10U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "Content-Length framing was accepted");

    auto crlf = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    for (std::size_t offset{}; (offset = crlf.find('\n', offset)) != std::string::npos;
         offset += 2U) {
        crlf.replace(offset, 1U, "\r\n");
    }
    processes.setOutput(std::move(crlf));
    requireError(verify(20U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "CRLF MCP response framing was accepted");

    processes.setOutput(
        " " + successfulResponse(Domain::LMStudioConnectorRole::Primary));
    requireError(verify(21U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "leading response whitespace was accepted");

    auto blankLine = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    blankLine.insert(blankLine.find('\n') + 1U, "\n");
    processes.setOutput(std::move(blankLine));
    requireError(verify(22U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a blank MCP response frame was accepted");

    auto duplicateTopLevel = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    const std::string topLevelId{"\"id\":1"};
    const auto topLevelIdOffset = duplicateTopLevel.find(topLevelId);
    require(topLevelIdOffset != std::string::npos,
            "the duplicate-key response seam was not found");
    duplicateTopLevel.replace(
        topLevelIdOffset, topLevelId.size(), topLevelId + ',' + topLevelId);
    processes.setOutput(std::move(duplicateTopLevel));
    requireError(verify(29U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a duplicate top-level MCP response key was accepted");

    auto duplicateDescriptor = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    const std::string canonicalName{"\"name\":\"agent_context\""};
    const auto canonicalNameOffset = duplicateDescriptor.find(canonicalName);
    require(canonicalNameOffset != std::string::npos,
            "the duplicate descriptor-key seam was not found");
    duplicateDescriptor.replace(
        canonicalNameOffset, canonicalName.size(), canonicalName + ',' + canonicalName);
    processes.setOutput(std::move(duplicateDescriptor));
    requireError(verify(30U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a duplicate nested MCP descriptor key was accepted");

    auto malformed = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    malformed.front() = '[';
    processes.setOutput(std::move(malformed));
    requireError(verify(31U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a malformed compact MCP response was accepted");

    auto excessiveDepth = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    excessiveDepth.insert(
        1U, "\"deep\":" + std::string(66U, '[') + '0' + std::string(66U, ']') + ',');
    processes.setOutput(std::move(excessiveDepth));
    requireError(verify(32U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an excessive-depth MCP response was accepted");

    std::string manyValues{"\"nodes\":["};
    for (std::size_t index{}; index < 17'000U; ++index) {
        if (index != 0U) {
            manyValues.push_back(',');
        }
        manyValues.push_back('0');
    }
    manyValues.append("],");
    auto excessiveEvents = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    excessiveEvents.insert(1U, manyValues);
    processes.setOutput(std::move(excessiveEvents));
    requireError(verify(33U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an excessive-node MCP response was accepted");

    auto extraFrame = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    extraFrame.append("{}\n");
    processes.setOutput(std::move(extraFrame));
    requireError(verify(34U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a third MCP response frame was accepted");

    auto missingFinalLf = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    missingFinalLf.pop_back();
    processes.setOutput(std::move(missingFinalLf));
    requireError(verify(23U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an MCP response without its final LF was accepted");

    auto pretty = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    const auto firstSeparator = pretty.find('\n');
    pretty = Json::parse(pretty.substr(0U, firstSeparator)).dump(2) +
        pretty.substr(firstSeparator);
    processes.setOutput(std::move(pretty));
    requireError(verify(24U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "pretty-printed MCP response JSON was accepted");

    auto outOfOrder = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    const auto replySeparator = outOfOrder.find('\n');
    const auto firstReply = outOfOrder.substr(0U, replySeparator + 1U);
    const auto secondReply = outOfOrder.substr(replySeparator + 1U);
    processes.setOutput(secondReply + firstReply);
    requireError(verify(25U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "out-of-order MCP response ids were accepted");

    auto invalidCapability = successfulResponse(Domain::LMStudioConnectorRole::Primary);
    const auto capabilitySeparator = invalidCapability.find('\n');
    auto initialize = Json::parse(invalidCapability.substr(0U, capabilitySeparator));
    initialize["result"]["capabilities"]["tools"] = Json::array();
    processes.setOutput(
        initialize.dump() + invalidCapability.substr(capabilitySeparator));
    requireError(verify(26U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a non-object initialize tools capability was accepted");

    processes.setOutput(mutateInitialize(
        successfulResponse(Domain::LMStudioConnectorRole::Primary),
        [](Json& response) {
            response["result"]["serverInfo"]["version"] = "0.9.1";
        }));
    requireError(verify(35U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a mismatched MCP server version was accepted");

    processes.setOutput(mutateInitialize(
        successfulResponse(Domain::LMStudioConnectorRole::Primary),
        [](Json& response) {
            response["result"]["capabilities"]["tools"]["listChanged"] = true;
        }));
    requireError(verify(36U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a dynamic MCP tool-list capability was accepted");

    processes.setOutput(mutateInitialize(
        successfulResponse(Domain::LMStudioConnectorRole::Primary),
        [](Json& response) {
            response["result"]["capabilities"].erase("projectMemory");
        }));
    requireError(verify(39U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an initialize reply without project-memory capability was accepted");

    processes.setOutput(mutateInitialize(
        successfulResponse(Domain::LMStudioConnectorRole::Primary),
        [](Json& response) {
            auto& titleBytes = response["result"]["capabilities"]["projectMemory"]
                                       ["limits"]["title_bytes"];
            titleBytes = titleBytes.get<std::size_t>() + 1U;
        }));
    requireError(verify(40U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "drifted initialize project-memory limits were accepted");

    processes.setOutput(mutateInitialize(
        successfulResponse(Domain::LMStudioConnectorRole::Primary),
        [](Json& response) {
            response["result"]["unexpected"] = true;
        }));
    requireError(verify(41U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an unknown initialize result property was accepted");

    processes.setOutput(successfulResponse(Domain::LMStudioConnectorRole::Fallback));
    requireError(verify(11U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "the wrong connector role was accepted");

    processes.setOutput(successfulResponse(Domain::LMStudioConnectorRole::Primary, 52U));
    requireError(verify(12U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a 52-tool MCP surface was accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, true, false));
    requireError(verify(13U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "duplicate MCP tool names were accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, false, true));
    requireError(verify(14U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "nondeterministic MCP tool ordering was accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, false, false, true, false));
    requireError(verify(15U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "an empty MCP tool description was accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, false, false, false, true));
    requireError(verify(16U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a non-object MCP input schema was accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, false, false, false, false,
        true, false));
    requireError(verify(27U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a wrong but sorted canonical MCP tool name was accepted");

    processes.setOutput(successfulResponse(
        Domain::LMStudioConnectorRole::Primary, 53U, false, false, false, false,
        false, true));
    requireError(verify(28U), Domain::ErrorCodes::HostCapabilityUnavailable,
                 "a compatible-looking canonical MCP schema drift was accepted");

    Domain::ProcessResult timedOut{};
    timedOut.timedOut = true;
    processes.setOutcome(timedOut);
    requireError(verify(17U), Domain::ErrorCodes::ProcessTimeout,
                 "a timed-out MCP smoke process was accepted");

    Domain::ProcessResult truncated{};
    truncated.stdoutTruncated = true;
    processes.setOutcome(truncated);
    requireError(verify(18U), Domain::ErrorCodes::PayloadTooLarge,
                 "a truncated MCP smoke response was accepted");

    Domain::ProcessResult oversized{};
    oversized.stdoutUtf8.assign(80'001U, 'x');
    processes.setOutcome(std::move(oversized));
    requireError(verify(37U), Domain::ErrorCodes::PayloadTooLarge,
                 "an oversized unflagged MCP smoke response was accepted");

    Domain::ProcessResult nonzero{};
    nonzero.exitCode = 17;
    processes.setOutcome(nonzero);
    requireError(verify(19U), Domain::ErrorCodes::ProcessExitNonzero,
                 "a nonzero MCP smoke process was accepted");
}

void testTargetedCancellationAndShutdownOwnership()
{
    ScriptedProcessSupervisor processes;
    WindowsLMStudioServeVerifier verifier{processes};
    const auto authority = AuthorityIssuer::create();
    const auto runVerification = [&](const Domain::OperationContext& context) {
        return verifier.verify(
            path("C:\\Forge\\forge-conductor.exe"),
            path("C:\\Forge\\home"),
            Domain::LMStudioConnectorRole::Primary,
            std::nullopt,
            authority,
            context);
    };

    processes.setBlocking();
    const auto cancelledContext = operationContext(30U);
    std::optional<Domain::Result<Domain::LMStudioConnectorHealth>> cancelled;
    std::jthread first{[&] { cancelled.emplace(runVerification(cancelledContext)); }};
    require(processes.waitUntilActive(), "the targeted verification did not become active");
    verifier.cancel(cancelledContext.operationId);
    first.join();
    require(cancelled.has_value(), "the targeted verification did not return");
    requireError(cancelled.value(), Domain::ErrorCodes::Cancelled,
                 "targeted verifier cancellation did not reach the process boundary");

    processes.setBlocking();
    const auto shutdownContext = operationContext(31U);
    std::optional<Domain::Result<Domain::LMStudioConnectorHealth>> shutDown;
    std::jthread second{[&] { shutDown.emplace(runVerification(shutdownContext)); }};
    require(processes.waitUntilActive(), "the shutdown verification did not become active");
    verifier.shutdown();
    second.join();
    require(shutDown.has_value(), "the shutdown verification did not return");
    requireError(shutDown.value(), Domain::ErrorCodes::Cancelled,
                 "verifier shutdown did not cancel its active process");
    require(!processes.processShutdown(),
            "the verifier shut down the injected shared process supervisor");
    const auto callsBeforeRejectedRun = processes.calls();
    requireError(runVerification(operationContext(32U)), Domain::ErrorCodes::Cancelled,
                 "the verifier accepted work after shutdown");
    require(processes.calls() == callsBeforeRejectedRun,
            "post-shutdown verification reached the process boundary");
    require(processes.cancelledOperations().size() == 2U,
            "targeted cancellation and shutdown did not remain operation-scoped");
}

void testCancellationSurvivesProcessPreAdmission()
{
    ScriptedProcessSupervisor processes;
    processes.setPreAdmissionBlocked();
    WindowsLMStudioServeVerifier verifier{processes};
    const auto authority = AuthorityIssuer::create();
    const auto cancelledContext = operationContext(33U);
    std::optional<Domain::Result<Domain::LMStudioConnectorHealth>> result;
    std::jthread verification{[&] {
        result.emplace(verifier.verify(
            path("C:\\Forge\\forge-conductor.exe"),
            path("C:\\Forge\\home"),
            Domain::LMStudioConnectorRole::Primary,
            std::nullopt,
            authority,
            cancelledContext));
    }};

    require(processes.waitUntilPreAdmission(),
            "the process pre-admission cancellation seam was not reached");
    verifier.cancel(cancelledContext.operationId);
    const bool cancellationObserved =
        processes.waitUntilPreAdmissionCancellation();
    if (!cancellationObserved) {
        processes.releasePreAdmission();
    }
    verification.join();

    require(cancellationObserved,
            "verifier cancellation was lost before process-supervisor admission");
    require(result.has_value(),
            "the pre-admission-cancelled verification did not return");
    requireError(result.value(), Domain::ErrorCodes::Cancelled,
                 "pre-admission verifier cancellation did not remain typed");
    require(processes.cancelledOperations().size() == 1U,
            "pre-admission verifier cancellation was not operation-scoped");
}

void testShutdownWaitsForActiveVerificationDrain()
{
    ScriptedProcessSupervisor processes;
    processes.setBlockingUntilReleased();
    WindowsLMStudioServeVerifier verifier{processes};
    const auto authority = AuthorityIssuer::create();
    const auto context = operationContext(38U);
    std::optional<Domain::Result<Domain::LMStudioConnectorHealth>> result;
    std::jthread verification{[&] {
        result.emplace(verifier.verify(
            path("C:\\Forge\\forge-conductor.exe"),
            path("C:\\Forge\\home"),
            Domain::LMStudioConnectorRole::Primary,
            std::nullopt,
            authority,
            context));
    }};

    const bool active = processes.waitUntilActive();
    if (!active) {
        processes.releaseBlocking();
        verification.join();
        require(false, "the drain verification did not become active");
    }

    std::mutex completionMutex;
    std::condition_variable completionChanged;
    bool shutdownReturned{};
    std::jthread shutdown{[&] {
        verifier.shutdown();
        {
            std::lock_guard lock{completionMutex};
            shutdownReturned = true;
        }
        completionChanged.notify_all();
    }};

    const bool cancellationIssued = processes.waitUntilCancellationCount(1U);
    bool returnedBeforeDrain{};
    {
        std::unique_lock lock{completionMutex};
        returnedBeforeDrain = completionChanged.wait_for(
            lock, 100ms, [&] { return shutdownReturned; });
    }

    processes.releaseBlocking();
    shutdown.join();
    verification.join();

    require(cancellationIssued,
            "draining shutdown did not issue operation-scoped cancellation");
    require(!returnedBeforeDrain,
            "verifier shutdown returned before its active operation drained");
    require(shutdownReturned && result.has_value(),
            "draining verifier shutdown or verification did not return");
    requireError(result.value(), Domain::ErrorCodes::Cancelled,
                 "the drained verification did not retain cancellation semantics");
}

} // namespace

void registerLMStudioServeVerifierTests(TestRegistry& tests)
{
    addTest(tests, "lmstudio.verifier.success-and-request-shape",
            testSuccessfulRoleVerificationAndRequestShape);
    addTest(tests, "lmstudio.verifier.fail-closed",
            testProtocolDriftAndProcessFailuresFailClosed);
    addTest(tests, "lmstudio.verifier.cancellation-and-shutdown",
            testTargetedCancellationAndShutdownOwnership);
    addTest(tests, "lmstudio.verifier.pre-admission-cancellation",
            testCancellationSurvivesProcessPreAdmission);
    addTest(tests, "lmstudio.verifier.shutdown-drain",
            testShutdownWaitsForActiveVerificationDrain);
}

} // namespace ForgeConductor::Tests
