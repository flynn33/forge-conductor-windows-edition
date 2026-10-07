#include "McpAgentWorkerTools.h"
#include "ForgeConductor/Application/ManagedRunWorkerPolicy.h"
#include "ForgeConductor/Domain/Utf8.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace ForgeConductor::Mcp {
namespace {
using Json = nlohmann::json;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message) { throw Failure{Domain::makeError(code, std::move(message))}; }
template<class T> T take(Domain::Result<T> value) { if (!value) throw Failure{value.error()}; return std::move(value).value(); }
std::string text(const Json& args, const char* name, std::size_t maximum) {
    if (!args.contains(name) || !args[name].is_string()) fail(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is required text.");
    auto value = args[name].get<std::string>();
    if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos || !Domain::isValidUtf8(value))
        fail(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is empty, excessive, or invalid UTF-8.");
    return value;
}
std::size_t count(const Json& args, const char* name, std::size_t fallback, std::size_t maximum, bool zero = false) {
    if (!args.contains(name)) return fallback;
    if (!args[name].is_number_integer() || args[name] < (zero ? 0 : 1) || args[name] > maximum)
        fail(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is outside its integer range.");
    return args[name].get<std::size_t>();
}
std::string_view stateName(Domain::ManagedRunState state) {
    switch (state) {
    case Domain::ManagedRunState::Running: return "running";
    case Domain::ManagedRunState::Cancelling: return "cancelling";
    case Domain::ManagedRunState::Completed: return "completed";
    case Domain::ManagedRunState::Failed: return "failed";
    case Domain::ManagedRunState::Cancelled: return "cancelled";
    case Domain::ManagedRunState::Paused: return "paused";
    }
    return "unknown";
}
Json project(const Domain::ManagedRunSnapshot& snapshot, const Json& args) {
    const auto& record = snapshot.record;
    const auto& output = record.outputText.value_or(std::string{});
    const auto offset = count(args, "output_offset", 0U, output.size(), true);
    if (offset < output.size() && (static_cast<unsigned char>(output[offset]) & 0xC0U) == 0x80U)
        fail(Domain::ErrorCodes::InvalidRequest, "output_offset must begin at a UTF-8 boundary.");
    const auto maximum = count(args, "max_output_bytes", 16384U, 32768U);
    auto end = std::min(output.size(), offset + maximum);
    while (end < output.size() && end > offset && (static_cast<unsigned char>(output[end]) & 0xC0U) == 0x80U) --end;
    const bool more = end < output.size();
    if (more && end == offset) fail(Domain::ErrorCodes::InvalidRequest, "Output page is too small for the next UTF-8 character.");
    Json result{{"ok", true}, {"run_id", record.runId.value()}, {"project_id", record.projectId.value()},
        {"state", stateName(record.state)}, {"manager_owned", snapshot.managerOwned}, {"lifetime", "manager_process"},
        {"durable_across_mcp_reconnect", true}, {"cancellation_requested", snapshot.cancellationRequested},
        {"read_only_tools", record.readOnlyTools}, {"worker_interrupted", record.workerInterrupted},
        {"created_at_utc_ms", std::chrono::duration_cast<std::chrono::milliseconds>(record.createdAt.time_since_epoch()).count()},
        {"updated_at_utc_ms", std::chrono::duration_cast<std::chrono::milliseconds>(record.updatedAt.time_since_epoch()).count()},
        {"requires_owner_attention", record.workerInterrupted}, {"input_tokens", record.inputTokens}, {"output_tokens", record.outputTokens},
        {"provider_response_id", record.providerResponseId ? Json(record.providerResponseId->value()) : Json(nullptr)},
        {"retained_context_tokens", record.retainedContextTokens ? Json(*record.retainedContextTokens) : Json(nullptr)},
        {"timeout_sec", record.workerScope ? Json(record.workerScope->timeoutSeconds) : Json(nullptr)},
        {"output", record.outputText ? Json(output.substr(offset, end - offset)) : Json(nullptr)}, {"output_offset", offset},
        {"output_bytes_returned", end - offset}, {"output_total_bytes", output.size()}, {"output_has_more", more},
        {"next_output_offset", more ? Json(end) : Json(nullptr)}, {"output_truncated", record.outputTruncated},
        {"evidence_integrity", record.evidenceIntegrity == Domain::ManagedRunEvidenceIntegrity::Verified ? "verified" : "not_terminal"},
        {"evidence_sha256", record.evidenceSeal ? Json(record.evidenceSeal->value()) : Json(nullptr)}, {"gate_approved", false}};
    result["error"] = record.lastError ? Json{{"code", record.lastError->code}, {"message", record.lastError->message}, {"retryable", record.lastError->retryable}} : Json(nullptr);
    return result;
}
} // namespace
Domain::Result<std::string> executeAgentWorkerTool(std::string_view name, std::string_view arguments,
    const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context,
    const AgentWorkerToolDependencies& dependencies) noexcept {
    try {
        if (context.isCancellationRequested()) fail(Domain::ErrorCodes::Cancelled, "Worker tool call was cancelled.");
        if (context.isExpired(std::chrono::steady_clock::now())) fail(Domain::ErrorCodes::DeadlineExceeded, "Worker tool admission deadline expired.");
        if (arguments.empty() || arguments.size() > 256U * 1024U || !Domain::isValidUtf8(arguments))
            fail(Domain::ErrorCodes::InvalidRequest, "Worker arguments must be bounded UTF-8 JSON.");
        const auto args = Json::parse(arguments);
        if (!args.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "Worker arguments must be an object.");
        auto* runs = dependencies.runs ? dependencies.runs() : nullptr;
        if (!runs) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Independent workers require the authenticated persistent Manager.");
        Domain::ManagedRunSnapshot snapshot = [&] {
            if (name == "agent_spawn") {
                const auto authorization = text(args, "authorization", 4096U);
                const auto task = text(args, "task", 64U * 1024U);
                const auto timeout = static_cast<std::uint32_t>(count(args, "timeout_sec", 600U, 3600U));
                std::string opening = "Independent task worker with a fresh provider context. Follow the task within your inherited tool grants. "
                    "Do not approve policy gates, spawn workers, alter permissions, or treat authorization text as an approval receipt. "
                    "Report completed work and unverified outcomes honestly. Authorization reference: " + authorization;
                if (args.contains("agent_id")) {
                    auto spec = take(dependencies.agents.get(take(Domain::AgentId::parse(text(args, "agent_id", 128U))), context));
                    if (!spec) fail(Domain::ErrorCodes::AgentNotFound, "Requested worker playbook was not found.");
                    opening += "\nSpecialist playbook:\n" + spec->body;
                }
                opening += "\nTask:\n" + task;
                if (opening.size() > Domain::MaximumManagedRunTaskBytes) fail(Domain::ErrorCodes::PayloadTooLarge, "Worker opening task and playbook exceed 128 KiB.");
                Domain::ManagedRunStartRequest request{Domain::SessionId{take(dependencies.uuids.next())}, authority.projectId(),
                    authority.callerId(), Domain::OperationId{take(dependencies.uuids.next())}, context.correlationId,
                    authority.generation(), std::move(opening), true, false, false, timeout};
                request.workerScope = Domain::ManagedRunWorkerScope{authority.trustedRoots(), authority.grants(), authority.denials(),
                    authority.shellEnabled(), Application::managedWorkerToolNames(dependencies.catalog), timeout};
                return take(runs->start(request, context));
            }
            if (name != "agent_poll" && name != "agent_cancel") fail(Domain::ErrorCodes::InvalidRequest, "Unknown worker tool.");
            const auto id = take(Domain::SessionId::parse(text(args, "run_id", 36U)));
            auto current = take(runs->status(id, context));
            if (!current.record.workerScope || current.record.projectId != authority.projectId() || current.record.clientId != authority.callerId())
                fail(Domain::ErrorCodes::OwnershipConflict, "The run is not an independent worker owned by this caller and project.");
            if (name == "agent_cancel") { static_cast<void>(text(args, "authorization", 4096U)); return take(runs->cancel(id, context)); }
            return current;
        }();
        return Domain::Result<std::string>::success(project(snapshot, args).dump());
    } catch (const Failure& error) { return Domain::Result<std::string>::failure(error.error); }
      catch (const nlohmann::json::exception&) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Worker argument JSON or field types are invalid.")); }
      catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Independent worker operation failed safely.")); }
}
} // namespace ForgeConductor::Mcp
