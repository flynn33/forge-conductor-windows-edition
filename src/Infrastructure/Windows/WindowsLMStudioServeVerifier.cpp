#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Detail/UtfConversion.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto VerificationTimeout = 15s;
constexpr std::size_t VerificationStdoutBytesMaximum = 80'000U;
constexpr std::size_t VerificationStderrBytesMaximum = 20'000U;
constexpr std::array<std::string_view, 53U> CanonicalToolNames{
    "agent_context",
    "agent_get",
    "agent_list",
    "agent_recommend",
    "agent_run_complete",
    "agent_run_start",
    "agent_run_status",
    "context_get",
    "context_list",
    "continuity.acknowledge_handoff",
    "continuity.checkpoint",
    "continuity.get_pending_handoff",
    "continuity.prepare_handoff",
    "continuity.request_rollover",
    "continuity.resume",
    "continuity.status",
    "forge_status",
    "fs_delete",
    "fs_edit",
    "fs_glob",
    "fs_list",
    "fs_mkdir",
    "fs_move",
    "fs_read",
    "fs_write",
    "git_add",
    "git_commit",
    "git_diff",
    "git_log",
    "git_status",
    "memory_delete",
    "memory_get",
    "memory_list",
    "memory_search",
    "memory_set",
    "pdf_from_file",
    "pdf_write",
    "project_memory.export",
    "project_memory.forget",
    "project_memory.get",
    "project_memory.import",
    "project_memory.initialize",
    "project_memory.link",
    "project_memory.list_recent",
    "project_memory.remember",
    "project_memory.remember_batch",
    "project_memory.search",
    "project_memory.status",
    "project_memory.update",
    "search_text",
    "session_checkpoint",
    "session_handoff",
    "shell_exec"};
constexpr std::size_t ExpectedToolCount = CanonicalToolNames.size();
// SHA-256 of the canonical compact `tools` array reviewed in
// tests/fixtures/Mcp/mcp-tools-semantic-golden.json. Production deliberately
// embeds the fingerprint and exact ordered names rather than loading a test
// fixture at runtime.
constexpr std::string_view CanonicalToolDescriptorSha256 =
    "b32131db479c2158f50f54c06f3652d8e880c49047b613c2bcbdcac31465753d";
constexpr std::size_t MaximumResponseFrames = 2U;
constexpr std::size_t MaximumJsonDepth = 64U;
constexpr std::size_t MaximumJsonEvents = 16'384U;
constexpr std::size_t MaximumToolNameBytes = 128U;
constexpr std::size_t MaximumToolDescriptionBytes = 4U * 1024U;
constexpr std::size_t MaximumInputSchemaBytes = 32U * 1024U;
constexpr std::size_t MaximumExecutableSearchPathCharacters =
    Domain::MaximumProcessEnvironmentValueBytes + 1U;
constexpr std::string_view RequestedProtocolVersion = "2025-11-25";
constexpr std::string_view ExpectedServerVersion = "0.9.0";

struct BoundedJsonRejected final {};

[[nodiscard]] std::string_view roleText(const Domain::LMStudioConnectorRole role) noexcept
{
    return role == Domain::LMStudioConnectorRole::Fallback ? "fallback" : "primary";
}

[[nodiscard]] std::string_view serverName(const Domain::LMStudioConnectorRole role) noexcept
{
    return role == Domain::LMStudioConnectorRole::Fallback ? "forge-conductor-fallback"
                                                            : "forge-conductor";
}

[[nodiscard]] Domain::Result<std::string> executableSearchPath() noexcept
{
    try {
        std::array<wchar_t, MaximumExecutableSearchPathCharacters> buffer{};
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetEnvironmentVariableW(
            L"PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The LM Studio serve verifier requires a bounded executable search path."));
        }
        if (length >= buffer.size()) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The LM Studio serve verifier executable search path exceeds its bound."));
        }
        auto converted = Detail::strictUtf16ToUtf8(
            std::wstring_view{buffer.data(), static_cast<std::size_t>(length)});
        if (!converted) {
            return converted;
        }
        if (converted.value().size() > Domain::MaximumProcessEnvironmentValueBytes) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The LM Studio serve verifier executable search path exceeds its process bound."));
        }
        return converted;
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The LM Studio serve verifier could not capture its executable search path."));
    }
}

[[nodiscard]] Json expectedInitializeResult(const Domain::LMStudioConnectorRole role)
{
    const Domain::ProjectMemoryLimits limits;
    return Json{
        {"capabilities",
         Json{
             {"projectMemory",
              Json{
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
                  {"schemaVersion", Domain::ProjectMemorySchemaVersion}}},
             {"tools", Json{{"listChanged", false}}}}},
        {"protocolVersion", RequestedProtocolVersion},
        {"serverInfo",
         Json{{"name", serverName(role)}, {"version", ExpectedServerVersion}}}};
}

[[nodiscard]] Domain::Error verificationFailure(std::string message)
{
    return Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, std::move(message));
}

[[nodiscard]] bool asciiCaseInsensitiveStartsWith(const std::string_view value,
                                                  const std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index{}; index < prefix.size(); ++index) {
        const auto lower = [](const char character) noexcept {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<char>(character - 'A' + 'a')
                       : character;
        };
        if (lower(value[index]) != lower(prefix[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Domain::Result<Json> parseBoundedJson(const std::string_view frame)
{
    try {
        std::size_t eventCount{};
        std::vector<std::set<std::string, std::less<>>> objectKeys;
        const auto callback = [&](const int depth, const Json::parse_event_t event, Json& value) {
            if (++eventCount > MaximumJsonEvents || depth < 0 ||
                static_cast<std::size_t>(depth) > MaximumJsonDepth) {
                throw BoundedJsonRejected{};
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() || !value.is_string() ||
                    !objectKeys.back().insert(value.get_ref<const std::string&>()).second) {
                    throw BoundedJsonRejected{};
                }
            } else if (event == Json::parse_event_t::object_end) {
                if (objectKeys.empty()) {
                    throw BoundedJsonRejected{};
                } else {
                    objectKeys.pop_back();
                }
            }
            return true;
        };
        auto parsed = Json::parse(frame.begin(), frame.end(), callback, false, false);
        if (!objectKeys.empty() || parsed.is_discarded() || !parsed.is_object()) {
            return Domain::Result<Json>::failure(verificationFailure(
                "The MCP serve smoke response was not one bounded JSON object."));
        }
        return Domain::Result<Json>::success(std::move(parsed));
    } catch (...) {
        return Domain::Result<Json>::failure(verificationFailure(
            "The MCP serve smoke response could not be parsed safely."));
    }
}

[[nodiscard]] Domain::Result<std::vector<Json>> parseResponseFrames(
    const std::string_view stdoutUtf8)
{
    try {
        if (stdoutUtf8.empty() || stdoutUtf8.size() > VerificationStdoutBytesMaximum ||
            !stdoutUtf8.ends_with('\n') ||
            stdoutUtf8.find('\r') != std::string_view::npos) {
            return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                "The MCP serve smoke response must use exact LF-terminated compact NDJSON."));
        }
        std::vector<Json> frames;
        frames.reserve(2U);
        std::size_t offset{};
        while (offset < stdoutUtf8.size()) {
            const auto separator = stdoutUtf8.find('\n', offset);
            if (separator == std::string_view::npos) {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke response ended without an LF frame terminator."));
            }
            const auto line = stdoutUtf8.substr(offset, separator - offset);
            if (line.empty() || line.front() == ' ' || line.front() == '\t') {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke response contained a blank or indented frame."));
            }
            if (asciiCaseInsensitiveStartsWith(line, "content-length:")) {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke response used forbidden Content-Length framing."));
            }
            if (frames.size() >= MaximumResponseFrames) {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke response exceeded its frame bound."));
            }
            auto parsed = parseBoundedJson(line);
            if (!parsed) {
                return Domain::Result<std::vector<Json>>::failure(
                    std::move(parsed).error());
            }
            if (parsed.value().dump() != line) {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke response was not compact deterministic JSON."));
            }
            frames.push_back(std::move(parsed).value());
            offset = separator + 1U;
        }
        if (frames.size() != 2U) {
            return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                "The MCP serve smoke response did not contain exactly two NDJSON replies."));
        }
        for (std::size_t index{}; index < frames.size(); ++index) {
            const auto id = frames[index].find("id");
            const auto expectedId = static_cast<std::int64_t>(index + 1U);
            if (id == frames[index].end() || !id->is_number_integer() ||
                id->get<std::int64_t>() != expectedId) {
                return Domain::Result<std::vector<Json>>::failure(verificationFailure(
                    "The MCP serve smoke replies were missing or out of request order."));
            }
        }
        return Domain::Result<std::vector<Json>>::success(std::move(frames));
    } catch (...) {
        return Domain::Result<std::vector<Json>>::failure(verificationFailure(
            "The MCP serve smoke response could not be bounded safely."));
    }
}

[[nodiscard]] const Json* responseFor(const std::vector<Json>& frames,
                                      const std::int64_t expectedId) noexcept
{
    const Json* match{};
    for (const auto& frame : frames) {
        const auto id = frame.find("id");
        if (id == frame.end() || !id->is_number_integer() || id->get<std::int64_t>() != expectedId) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &frame;
    }
    return match;
}

[[nodiscard]] Domain::Result<std::string> validateInitializeResponse(
    const std::vector<Json>& frames, const Domain::LMStudioConnectorRole role)
{
    try {
        const Json* response = responseFor(frames, 1);
        if (response == nullptr || response->size() != 3U ||
            response->value("jsonrpc", std::string{}) != "2.0" ||
            response->contains("error") || !response->contains("result") ||
            !response->at("result").is_object()) {
            return Domain::Result<std::string>::failure(verificationFailure(
                "The MCP serve initialize reply was missing, duplicated, or unsuccessful."));
        }
        const auto& result = response->at("result");
        if (!result.contains("protocolVersion") || !result.at("protocolVersion").is_string() ||
            result.at("protocolVersion").get_ref<const std::string&>() !=
                RequestedProtocolVersion ||
            !result.contains("serverInfo") || !result.at("serverInfo").is_object()) {
            return Domain::Result<std::string>::failure(verificationFailure(
                "The MCP serve initialize reply did not negotiate the required protocol."));
        }
        const auto& info = result.at("serverInfo");
        if (!info.contains("name") || !info.at("name").is_string() ||
            info.at("name").get_ref<const std::string&>() != serverName(role) ||
            !info.contains("version") || !info.at("version").is_string() ||
            info.at("version").get_ref<const std::string&>() != ExpectedServerVersion) {
            return Domain::Result<std::string>::failure(verificationFailure(
                "The MCP serve initialize reply identified the wrong connector role or product version."));
        }
        if (!result.contains("capabilities") || !result.at("capabilities").is_object() ||
            !result.at("capabilities").contains("tools") ||
            !result.at("capabilities").at("tools").is_object() ||
            !result.at("capabilities").at("tools").contains("listChanged") ||
            !result.at("capabilities").at("tools").at("listChanged").is_boolean() ||
            result.at("capabilities").at("tools").at("listChanged").get<bool>()) {
            return Domain::Result<std::string>::failure(verificationFailure(
                "The MCP serve initialize reply did not expose the exact static tools capability."));
        }
        if (result != expectedInitializeResult(role)) {
            return Domain::Result<std::string>::failure(verificationFailure(
                "The MCP serve initialize reply drifted from the exact product capabilities."));
        }
        return Domain::Result<std::string>::success(
            result.at("protocolVersion").get<std::string>());
    } catch (...) {
        return Domain::Result<std::string>::failure(verificationFailure(
            "The MCP serve initialize reply had an invalid structure."));
    }
}

[[nodiscard]] Domain::Result<std::size_t> validateToolResponse(
    const std::vector<Json>& frames)
{
    try {
        const Json* response = responseFor(frames, 2);
        if (response == nullptr || response->size() != 3U ||
            response->value("jsonrpc", std::string{}) != "2.0" ||
            response->contains("error") || !response->contains("result") ||
            !response->at("result").is_object() ||
            response->at("result").size() != 1U ||
            !response->at("result").contains("tools") ||
            !response->at("result").at("tools").is_array()) {
            return Domain::Result<std::size_t>::failure(verificationFailure(
                "The MCP tools/list reply was missing, duplicated, or unsuccessful."));
        }
        const auto& tools = response->at("result").at("tools");
        if (tools.size() != ExpectedToolCount) {
            return Domain::Result<std::size_t>::failure(verificationFailure(
                "The MCP tools/list reply did not expose exactly 53 tools."));
        }
        for (std::size_t index{}; index < tools.size(); ++index) {
            const auto& tool = tools[index];
            if (!tool.is_object() || !tool.contains("name") || !tool.at("name").is_string() ||
                !tool.contains("description") || !tool.at("description").is_string() ||
                !tool.contains("inputSchema") || !tool.at("inputSchema").is_object()) {
                return Domain::Result<std::size_t>::failure(verificationFailure(
                    "The MCP tools/list reply contained an incomplete tool descriptor."));
            }
            const auto& name = tool.at("name").get_ref<const std::string&>();
            const auto& description =
                tool.at("description").get_ref<const std::string&>();
            const auto& schema = tool.at("inputSchema");
            if (name.empty() || name.size() > MaximumToolNameBytes ||
                !Domain::isValidUtf8(name) || name != CanonicalToolNames[index]) {
                return Domain::Result<std::size_t>::failure(verificationFailure(
                    "The MCP tools/list reply did not match the exact canonical tool inventory."));
            }
            if (description.empty() || description.size() > MaximumToolDescriptionBytes ||
                description.find('\0') != std::string::npos ||
                !Domain::isValidUtf8(description)) {
                return Domain::Result<std::size_t>::failure(verificationFailure(
                    "The MCP tools/list reply contained an invalid tool description."));
            }
            if (schema.value("type", std::string{}) != "object" ||
                schema.dump().size() > MaximumInputSchemaBytes) {
                return Domain::Result<std::size_t>::failure(verificationFailure(
                    "The MCP tools/list reply contained an invalid object input schema."));
            }
        }
        const auto canonical = tools.dump();
        BCryptSha256Hasher hasher;
        const auto digest = hasher.sha256(std::as_bytes(std::span{canonical}));
        if (!digest || digest.value().value() != CanonicalToolDescriptorSha256) {
            return Domain::Result<std::size_t>::failure(verificationFailure(
                "The MCP tools/list reply descriptor schemas or semantics drifted from the canonical inventory."));
        }
        return Domain::Result<std::size_t>::success(tools.size());
    } catch (...) {
        return Domain::Result<std::size_t>::failure(verificationFailure(
            "The MCP tools/list reply had an invalid structure."));
    }
}

[[nodiscard]] std::string requestStream()
{
    const Json initialize{
        {"id", 1},
        {"jsonrpc", "2.0"},
        {"method", "initialize"},
        {"params",
         Json{{"capabilities", Json::object()},
              {"clientInfo", Json{{"name", "forge-conductor-lmstudio-smoke"},
                                   {"version", "0.9.0"}}},
              {"protocolVersion", RequestedProtocolVersion}}}};
    const Json initialized{{"jsonrpc", "2.0"},
                           {"method", "notifications/initialized"},
                           {"params", Json::object()}};
    const Json tools{{"id", 2},
                     {"jsonrpc", "2.0"},
                     {"method", "tools/list"},
                     {"params", Json::object()}};
    return initialize.dump() + '\n' + initialized.dump() + '\n' + tools.dump() + '\n';
}

} // namespace

class WindowsLMStudioServeVerifier::Impl final {
public:
    explicit Impl(Contracts::IProcessSupervisor& processSupervisor) noexcept
        : processSupervisor_{processSupervisor}
    {}

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectorHealth> verify(
        const Domain::PathText& binaryPath,
        const Domain::PathText& forgeHome,
        const Domain::LMStudioConnectorRole role,
        const std::optional<Domain::DeploymentId>& deploymentId,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Domain::OperationContext& context)
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The LM Studio serve verification was cancelled before admission."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The LM Studio serve verification deadline expired before admission."));
        }
        std::shared_ptr<ActiveVerification> activeVerification;
        {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The LM Studio serve verifier has shut down."));
            }
            if (activeOperations_.size() >= Contracts::IProcessSupervisor::MaximumConcurrentOperations) {
                return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The LM Studio serve verifier reached its concurrent operation bound."));
            }
            const auto duplicate = std::find_if(
                activeOperations_.begin(), activeOperations_.end(),
                [&](const auto& active) {
                    return active->operationId == context.operationId;
                });
            if (duplicate != activeOperations_.end()) {
                return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The LM Studio serve verification operation is already active."));
            }
            activeVerification =
                std::make_shared<ActiveVerification>(context.operationId);
            activeOperations_.push_back(activeVerification);
        }
        struct Release final {
            Impl& owner;
            const std::shared_ptr<ActiveVerification>& operation;
            ~Release() noexcept { owner.release(operation); }
        } release{*this, activeVerification};

        std::stop_callback callerCancellation{
            context.cancellation,
            [activeVerification] {
                static_cast<void>(activeVerification->cancellation.request_stop());
            }};
        const Domain::OperationContext effectiveContext{
            context.operationId,
            context.deadline,
            activeVerification->cancellation.get_token(),
            context.correlationId};

        auto searchPath = executableSearchPath();
        if (!searchPath) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                std::move(searchPath).error());
        }

        Domain::ProcessRequest request{binaryPath};
        request.arguments = {"serve"};
        // The serve composition root adopts its current directory as a durable
        // project. Binding that directory to Forge home prevents a smoke probe
        // from inheriting and persisting an unrelated caller workspace, and it
        // makes the process supervisor authorize the data root as a launch path.
        request.workingDirectory = forgeHome;
        request.environment = {
            Domain::EnvironmentVariable{"FORGE_CONDUCTOR_HOME", forgeHome.value()},
            Domain::EnvironmentVariable{"FORGE_MCP_ROLE", std::string{roleText(role)}},
            // An explicit empty overlay suppresses an ambient parent revision;
            // the serve composition root then creates an isolated probe ID.
            Domain::EnvironmentVariable{
                "FORGE_DEPLOYMENT_ID", deploymentId ? deploymentId->value() : std::string{}},
            Domain::EnvironmentVariable{"PATH", std::move(searchPath).value()}};
        request.inheritEnvironment = true;
        request.timeout = VerificationTimeout;
        request.maximumStdoutBytes = VerificationStdoutBytesMaximum;
        request.maximumStderrBytes = VerificationStderrBytesMaximum;
        request.stdinUtf8 = requestStream();

        if (activeVerification->cancellation.stop_requested()) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The LM Studio serve verification was cancelled before process admission."));
        }
        auto process = processSupervisor_.run(
            request, executionAuthority, effectiveContext);
        if (!process) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                std::move(process).error());
        }
        if (activeVerification->cancellation.stop_requested()) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The LM Studio serve verification was cancelled."));
        }
        const auto& outcome = process.value();
        if (!outcome.terminationConfirmed) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::ProcessTerminationUnconfirmed,
                "The MCP serve smoke process did not confirm process-tree termination."));
        }
        if (outcome.cancelled) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The MCP serve smoke process was cancelled."));
        }
        if (outcome.timedOut) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::ProcessTimeout,
                "The MCP serve smoke process exceeded its bounded timeout."));
        }
        if (outcome.stdoutTruncated || outcome.stderrTruncated ||
            outcome.stdoutUtf8.size() > VerificationStdoutBytesMaximum ||
            outcome.stderrUtf8.size() > VerificationStderrBytesMaximum) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The MCP serve smoke process exceeded its bounded output capture."));
        }
        if (outcome.exitCode != 0) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::ProcessExitNonzero,
                "The MCP serve smoke process returned exit code " +
                    std::to_string(outcome.exitCode) + "."));
        }

        auto frames = parseResponseFrames(outcome.stdoutUtf8);
        if (!frames) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                std::move(frames).error());
        }
        auto protocol = validateInitializeResponse(frames.value(), role);
        if (!protocol) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                std::move(protocol).error());
        }
        auto toolCount = validateToolResponse(frames.value());
        if (!toolCount) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                std::move(toolCount).error());
        }
        return Domain::Result<Domain::LMStudioConnectorHealth>::success(
            Domain::LMStudioConnectorHealth{
                role,
                true,
                std::move(protocol).value(),
                toolCount.value(),
                std::string{serverName(role)} +
                    " passed bounded NDJSON initialize and tools/list verification."});
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::shared_ptr<ActiveVerification> active;
            {
                std::lock_guard lock{mutex_};
                const auto match = std::find_if(
                    activeOperations_.begin(), activeOperations_.end(),
                    [&](const auto& candidate) {
                        return candidate->operationId == operationId;
                    });
                if (match != activeOperations_.end()) {
                    active = *match;
                }
            }
            if (active) {
                static_cast<void>(active->cancellation.request_stop());
                processSupervisor_.cancel(operationId);
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            std::array<
                std::shared_ptr<ActiveVerification>,
                Contracts::IProcessSupervisor::MaximumConcurrentOperations> active{};
            std::size_t activeCount{};
            {
                std::lock_guard lock{mutex_};
                shutdown_ = true;
                for (const auto& operation : activeOperations_) {
                    static_cast<void>(operation->cancellation.request_stop());
                    active[activeCount++] = operation;
                }
            }
            for (std::size_t index{}; index < activeCount; ++index) {
                processSupervisor_.cancel(active[index]->operationId);
            }
            std::unique_lock lock{mutex_};
            operationChanged_.wait(lock, [&] { return activeOperations_.empty(); });
        } catch (...) {
        }
    }

private:
    struct ActiveVerification final {
        explicit ActiveVerification(Domain::OperationId value)
            : operationId{std::move(value)}
        {
        }

        Domain::OperationId operationId;
        std::stop_source cancellation;
    };

    void release(
        const std::shared_ptr<ActiveVerification>& operation) noexcept
    {
        bool changed{};
        try {
            {
                std::lock_guard lock{mutex_};
                const auto match = std::find(
                    activeOperations_.begin(), activeOperations_.end(), operation);
                if (match != activeOperations_.end()) {
                    activeOperations_.erase(match);
                    changed = true;
                }
            }
            if (changed) {
                operationChanged_.notify_all();
            }
        } catch (...) {
        }
    }

    Contracts::IProcessSupervisor& processSupervisor_;
    std::mutex mutex_;
    std::condition_variable operationChanged_;
    std::vector<std::shared_ptr<ActiveVerification>> activeOperations_;
    bool shutdown_{};
};

WindowsLMStudioServeVerifier::WindowsLMStudioServeVerifier(
    Contracts::IProcessSupervisor& processSupervisor) noexcept
{
    try {
        implementation_ = std::make_shared<Impl>(processSupervisor);
    } catch (...) {
    }
}

WindowsLMStudioServeVerifier::~WindowsLMStudioServeVerifier()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<Domain::LMStudioConnectorHealth> WindowsLMStudioServeVerifier::verify(
    const Domain::PathText& binaryPath,
    const Domain::PathText& forgeHome,
    const Domain::LMStudioConnectorRole role,
    const std::optional<Domain::DeploymentId>& deploymentId,
    const Contracts::WorkspaceAuthority& executionAuthority,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The LM Studio serve verifier could not initialize or is no longer available."));
        }
        return implementation->verify(binaryPath, forgeHome, role, deploymentId,
                                      executionAuthority, context);
    } catch (const std::exception& exception) {
        return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{"The LM Studio serve verifier failed: "} + exception.what()));
    } catch (...) {
        return Domain::Result<Domain::LMStudioConnectorHealth>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The LM Studio serve verifier failed with an unknown exception."));
    }
}

void WindowsLMStudioServeVerifier::cancel(const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancel(operationId);
    }
}

void WindowsLMStudioServeVerifier::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
