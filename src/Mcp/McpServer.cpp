#include "ForgeConductor/Mcp/McpServer.h"

#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpProtocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

using Json = nlohmann::json;

constexpr std::string_view ProductVersion = "0.9.0";
constexpr std::string_view PrimaryServerName = "forge-conductor";
constexpr std::string_view FallbackServerName = "forge-conductor-fallback";

[[nodiscard]] Json jsonRpcError(
    const std::optional<Json>& id,
    const int code,
    std::string message)
{
    return Json{
        {"error", Json{{"code", code}, {"message", std::move(message)}}},
        {"id", id.value_or(Json(nullptr))},
        {"jsonrpc", "2.0"}};
}

[[nodiscard]] Json jsonRpcError(
    const Json& id,
    const int code,
    std::string message)
{
    return jsonRpcError(
        std::optional<Json>{id}, code, std::move(message));
}

[[nodiscard]] Json jsonRpcResult(const Json& id, Json result)
{
    return Json{
        {"id", id},
        {"jsonrpc", "2.0"},
        {"result", std::move(result)}};
}

[[nodiscard]] Json stableErrorPayload(const Domain::Error& error)
{
    Json payload{
        {"code", error.code},
        {"message", error.message},
        {"ok", false},
        {"retryable", error.retryable}};
    if (error.evidenceId) {
        payload["evidence_id"] = error.evidenceId.value();
    }
    return payload;
}

[[nodiscard]] Json toolEnvelope(Json payload, const bool isError)
{
    const auto text = payload.dump();
    return Json{
        {"content", Json::array({Json{{"text", text}, {"type", "text"}}})},
        {"isError", isError},
        {"structuredContent", std::move(payload)}};
}

[[nodiscard]] std::size_t encodedJsonStringBytes(
    const std::string_view value) noexcept
{
    std::size_t bytes{2U};
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            bytes += 2U;
            break;
        default:
            bytes += character <= 0x1FU ? 6U : 1U;
            break;
        }
    }
    return bytes;
}

[[nodiscard]] Json domainFailureResponse(
    const Json& id,
    const Domain::Error& error)
{
    return jsonRpcResult(id, toolEnvelope(stableErrorPayload(error), true));
}

[[nodiscard]] std::optional<std::string> requestKey(const Json& value)
{
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > McpServer::MaximumRequestIdBytes) {
            return std::nullopt;
        }
        return std::string{"s:"} + text;
    }
    if (value.is_number()) {
        return std::string{"n:"} + value.dump();
    }
    return std::nullopt;
}

[[nodiscard]] bool validRequestId(const Json& value) noexcept
{
    return value.is_null() || value.is_number() ||
        (value.is_string() &&
         value.get_ref<const std::string&>().size() <=
             McpServer::MaximumRequestIdBytes);
}

[[nodiscard]] Json projectMemoryLimits()
{
    const Domain::ProjectMemoryLimits limits;
    return Json{
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
        {"title_bytes", limits.maximumTitleBytes}};
}

[[nodiscard]] Json initializeResult(
    const Domain::McpRole role,
    const std::string_view protocolVersion)
{
    return Json{
        {"capabilities",
         Json{
             {"projectMemory",
              Json{
                  {"capabilityVersion", Domain::ProjectMemoryCapabilityVersion},
                  {"limits", projectMemoryLimits()},
                  {"schemaVersion", Domain::ProjectMemorySchemaVersion}}},
             {"tools", Json{{"listChanged", false}}}}},
        {"protocolVersion", protocolVersion},
        {"serverInfo",
         Json{
             {"name",
              role == Domain::McpRole::Fallback
                  ? FallbackServerName
                  : PrimaryServerName},
             {"version", ProductVersion}}}};
}

[[nodiscard]] Domain::Error internalError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::move(message));
}

[[nodiscard]] Domain::Error invalidToolRequest(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        std::move(message));
}

} // namespace

class McpServer::Implementation final {
public:
    Implementation(
        Contracts::IToolCatalog& catalog,
        Contracts::IToolRouter& router,
        Contracts::IMcpExecutionContextResolver& contextResolver,
        Contracts::IUuidGenerator& uuidGenerator,
        const Contracts::IClock& clock) noexcept
        : catalog_{catalog},
          router_{router},
          contextResolver_{contextResolver},
          uuidGenerator_{uuidGenerator},
          clock_{clock}
    {
    }

    [[nodiscard]] Domain::Result<void> run(
        Contracts::IMcpTransport& transport,
        const Domain::McpRole role,
        const Domain::DeploymentId& deploymentId,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        std::jthread worker;
        try {
            {
                std::lock_guard lock{stateMutex_};
                if (running_ || permanentlyShutdown_) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The MCP server is already running or has shut down."));
                }
                running_ = true;
                stopping_ = false;
                inputClosed_ = false;
                transport_ = &transport;
                terminalError_.reset();
                negotiatedProtocol_ =
                    std::string{McpProtocol::SupportedVersions.front()};
            }

            static_cast<void>(deploymentId);
            worker = std::jthread{
                [this, &transport](const std::stop_token token) {
                    workerLoop(transport, token);
                }};

            bool cleanEndOfStream{};
            while (true) {
                {
                    std::lock_guard lock{stateMutex_};
                    if (stopping_) {
                        break;
                    }
                }
                if (context.isCancellationRequested() ||
                    context.isExpired(clock_.monotonicNow())) {
                    setTerminalFailure(Domain::makeError(
                        context.isCancellationRequested()
                            ? Domain::ErrorCodes::Cancelled
                            : Domain::ErrorCodes::DeadlineExceeded,
                        "The MCP server operation ended before input completed."));
                    requestStop(true);
                    break;
                }

                auto received = transport.receive(context);
                if (!received) {
                    const auto error = received.error();
                    if (error.code == Domain::ErrorCodes::MalformedMessage ||
                        error.code == Domain::ErrorCodes::PayloadTooLarge) {
                        sendResponse(
                            transport,
                            jsonRpcError(
                                std::nullopt,
                                error.code == Domain::ErrorCodes::MalformedMessage
                                    ? -32700
                                    : -32000,
                                error.code == Domain::ErrorCodes::MalformedMessage
                                    ? "Parse error"
                                    : "MCP input exceeds the configured limit."),
                            context);
                        continue;
                    }
                    bool alreadyStopping{};
                    {
                        std::lock_guard lock{stateMutex_};
                        alreadyStopping = stopping_;
                    }
                    if (!alreadyStopping) {
                        setTerminalFailure(std::move(received).error());
                        requestStop(true);
                    }
                    break;
                }
                if (!received.value()) {
                    cleanEndOfStream = true;
                    {
                        std::lock_guard lock{stateMutex_};
                        inputClosed_ = true;
                    }
                    stateChanged_.notify_all();
                    break;
                }

                handleFrame(
                    received.value().value(),
                    transport,
                    role,
                    clientId,
                    context);
            }

            if (!cleanEndOfStream) {
                worker.request_stop();
                stateChanged_.notify_all();
            }
            if (worker.joinable()) {
                worker.join();
            }

            router_.shutdown();
            transport.shutdown();

            std::optional<Domain::Error> terminalError;
            {
                std::lock_guard lock{stateMutex_};
                terminalError = terminalError_;
                permanentlyShutdown_ = true;
                running_ = false;
                stopping_ = true;
                inputClosed_ = true;
                transport_ = nullptr;
                pendingCalls_.clear();
                requests_.clear();
                preCancelled_.clear();
                preCancellationOrder_.clear();
            }
            stateChanged_.notify_all();
            if (terminalError) {
                return Domain::Result<void>::failure(std::move(terminalError.value()));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            requestStop(true);
            router_.shutdown();
            worker.request_stop();
            stateChanged_.notify_all();
            if (worker.joinable()) {
                worker.join();
            }
            transport.shutdown();
            {
                std::lock_guard lock{stateMutex_};
                permanentlyShutdown_ = true;
                running_ = false;
                transport_ = nullptr;
            }
            stateChanged_.notify_all();
            return Domain::Result<void>::failure(internalError(
                "The MCP server failed at its process boundary."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            bool matched{};
            {
                std::lock_guard lock{stateMutex_};
                for (auto& [key, request] : requests_) {
                    static_cast<void>(key);
                    if (request.operationId == operationId) {
                        request.cancellation.request_stop();
                        matched = true;
                    }
                }
            }
            if (matched) {
                router_.cancel(operationId);
                stateChanged_.notify_all();
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            Contracts::IMcpTransport* transport{};
            {
                std::lock_guard lock{stateMutex_};
                permanentlyShutdown_ = true;
                transport = transport_;
            }
            requestStop(true);
            router_.shutdown();
            if (transport != nullptr) {
                transport->shutdown();
            }
        } catch (...) {
        }
    }

    void waitForRunCompletion() noexcept
    {
        try {
            std::unique_lock lock{stateMutex_};
            stateChanged_.wait(lock, [this] { return !running_; });
        } catch (...) {
        }
    }

private:
    struct RequestControl final {
        RequestControl(
            Domain::OperationId id,
            std::stop_source source) noexcept
            : operationId{std::move(id)}, cancellation{std::move(source)}
        {
        }

        Domain::OperationId operationId;
        std::stop_source cancellation;
    };

    struct PendingCall final {
        Json externalId;
        std::string externalKey;
        Domain::ToolCallRequest request;
        Domain::ToolEffect effect{Domain::ToolEffect::Read};
        Domain::OperationId operationId;
        Domain::CorrelationId correlationId;
        std::stop_source cancellation;
        Domain::MonotonicTimePoint deadline;
        Domain::OperationContext transportContext;
    };

    struct GeneratedIds final {
        Domain::OperationId operationId;
        Domain::RequestId requestId;
        Domain::CorrelationId correlationId;
    };

    void handleFrame(
        const Domain::McpFrame& frame,
        Contracts::IMcpTransport& transport,
        const Domain::McpRole role,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& transportContext)
    {
        const McpJsonCodec codec;
        auto canonical = codec.canonicalize(frame.utf8Json);
        if (!canonical) {
            const auto& error = canonical.error();
            sendResponse(
                transport,
                jsonRpcError(
                    std::nullopt,
                    error.code == Domain::ErrorCodes::LimitExceeded ||
                            error.code == Domain::ErrorCodes::PayloadTooLarge
                        ? -32000
                        : error.code == Domain::ErrorCodes::InvalidRequest
                            ? -32600
                            : error.code == Domain::ErrorCodes::InternalFailure
                                ? -32603
                        : -32700,
                    error.code == Domain::ErrorCodes::LimitExceeded
                        ? "MCP input exceeds the nesting-depth limit."
                        : error.code == Domain::ErrorCodes::PayloadTooLarge
                            ? "MCP input exceeds the configured limit."
                            : error.code == Domain::ErrorCodes::InvalidRequest
                                ? "Invalid Request"
                                : error.code == Domain::ErrorCodes::InternalFailure
                                    ? "Internal error"
                            : "Parse error"),
                transportContext);
            return;
        }
        auto parsed = Json::parse(canonical.value());

        const auto idIterator = parsed.find("id");
        const bool hasId = idIterator != parsed.end() && !idIterator->is_null();
        const std::optional<Json> responseId =
            idIterator == parsed.end()
                ? std::nullopt
                : std::optional<Json>{*idIterator};
        if (idIterator != parsed.end() && !validRequestId(*idIterator)) {
            sendResponse(
                transport,
                jsonRpcError(std::nullopt, -32600, "Invalid Request"),
                transportContext);
            return;
        }

        const auto version = parsed.find("jsonrpc");
        const auto method = parsed.find("method");
        if (version == parsed.end() || !version->is_string() ||
            version->get_ref<const std::string&>() != "2.0" ||
            method == parsed.end() || !method->is_string() ||
            method->get_ref<const std::string&>().empty() ||
            method->get_ref<const std::string&>().size() >
                McpServer::MaximumMethodNameBytes) {
            if (hasId || method == parsed.end()) {
                sendResponse(
                    transport,
                    jsonRpcError(responseId, -32600, "Invalid Request"),
                    transportContext);
            }
            return;
        }

        const auto& methodName = method->get_ref<const std::string&>();
        if (methodName == "notifications/cancelled") {
            handleCancellation(parsed);
            return;
        }
        if (methodName.starts_with("notifications/") || !hasId) {
            return;
        }

        const Json& externalId = *idIterator;
        if (consumePreCancellation(externalId)) {
            sendResponse(
                transport,
                jsonRpcError(externalId, -32800, "Cancelled"),
                transportContext);
            return;
        }
        if (methodName == "initialize") {
            std::string requested;
            const auto params = parsed.find("params");
            if (params != parsed.end() && params->is_object()) {
                const auto requestedVersion = params->find("protocolVersion");
                if (requestedVersion != params->end() &&
                    requestedVersion->is_string()) {
                    requested = requestedVersion->get<std::string>();
                }
            }
            const auto negotiated = McpProtocol::negotiate(requested);
            {
                std::lock_guard lock{stateMutex_};
                negotiatedProtocol_ = std::string{negotiated};
            }
            sendResponse(
                transport,
                jsonRpcResult(
                    externalId,
                    initializeResult(role, negotiated)),
                transportContext);
            return;
        }
        if (methodName == "ping") {
            sendResponse(
                transport,
                jsonRpcResult(externalId, Json::object()),
                transportContext);
            return;
        }
        if (methodName == "tools/list") {
            auto listed = toolsList();
            if (!listed) {
                sendResponse(
                    transport,
                    jsonRpcError(
                        responseId,
                        -32000,
                        listed.error().message),
                    transportContext);
                return;
            }
            sendResponse(
                transport,
                jsonRpcResult(
                    externalId,
                    Json{{"tools", std::move(listed).value()}}),
                transportContext);
            return;
        }
        if (methodName == "resources/list") {
            sendResponse(
                transport,
                jsonRpcResult(
                    externalId,
                    Json{{"resources", Json::array()}}),
                transportContext);
            return;
        }
        if (methodName == "prompts/list") {
            sendResponse(
                transport,
                jsonRpcResult(
                    externalId,
                    Json{{"prompts", Json::array()}}),
                transportContext);
            return;
        }
        if (methodName == "tools/call") {
            enqueueToolCall(
                parsed,
                externalId,
                transport,
                clientId,
                transportContext);
            return;
        }

        sendResponse(
            transport,
            jsonRpcError(
                responseId,
                -32601,
                "Method not found"),
            transportContext);
    }

    [[nodiscard]] bool consumePreCancellation(const Json& externalId)
    {
        const auto key = requestKey(externalId);
        if (!key) {
            return false;
        }
        std::lock_guard lock{stateMutex_};
        const auto cancelled = preCancelled_.find(key.value());
        if (cancelled == preCancelled_.end()) {
            return false;
        }
        preCancelled_.erase(cancelled);
        std::erase(preCancellationOrder_, key.value());
        return true;
    }

    void handleCancellation(const Json& message)
    {
        const auto params = message.find("params");
        if (params == message.end() || !params->is_object()) {
            return;
        }
        auto identifier = params->find("requestId");
        if (identifier == params->end()) {
            identifier = params->find("request_id");
        }
        if (identifier == params->end()) {
            return;
        }
        const auto key = requestKey(*identifier);
        if (!key) {
            return;
        }

        std::optional<Domain::OperationId> operation;
        {
            std::lock_guard lock{stateMutex_};
            const auto request = requests_.find(key.value());
            if (request != requests_.end()) {
                request->second.cancellation.request_stop();
                operation = request->second.operationId;
            } else if (preCancelled_.insert(key.value()).second) {
                preCancellationOrder_.push_back(key.value());
                while (preCancellationOrder_.size() >
                       McpServer::MaximumPreCancellationIds) {
                    preCancelled_.erase(preCancellationOrder_.front());
                    preCancellationOrder_.pop_front();
                }
            }
        }
        if (operation) {
            router_.cancel(operation.value());
            stateChanged_.notify_all();
        }
    }

    void enqueueToolCall(
        const Json& message,
        const Json& externalId,
        Contracts::IMcpTransport& transport,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& transportContext)
    {
        const auto key = requestKey(externalId);
        if (!key) {
            sendResponse(
                transport,
                jsonRpcError(externalId, -32600, "Invalid Request"),
                transportContext);
            return;
        }

        const auto params = message.find("params");
        if (params == message.end() || !params->is_object()) {
            sendResponse(
                transport,
                jsonRpcError(externalId, -32600, "Invalid Request"),
                transportContext);
            return;
        }
        const auto name = params->find("name");
        if (name == params->end() || !name->is_string() ||
            name->get_ref<const std::string&>().empty() ||
            name->get_ref<const std::string&>().size() >
                McpServer::MaximumToolNameBytes) {
            sendResponse(
                transport,
                jsonRpcError(externalId, -32600, "Invalid Request"),
                transportContext);
            return;
        }
        Json arguments = Json::object();
        const auto suppliedArguments = params->find("arguments");
        if (suppliedArguments != params->end()) {
            if (!suppliedArguments->is_object()) {
                sendResponse(
                    transport,
                    jsonRpcError(externalId, -32600, "Invalid Request"),
                    transportContext);
                return;
            }
            arguments = *suppliedArguments;
        }

        const auto descriptor = std::find_if(
            catalog_.tools().begin(),
            catalog_.tools().end(),
            [&](const Domain::McpToolDescriptor& candidate) {
                return candidate.tool.name == name->get_ref<const std::string&>();
            });
        if (descriptor == catalog_.tools().end()) {
            sendResponse(
                transport,
                domainFailureResponse(
                    externalId,
                    invalidToolRequest("Unknown tool.")),
                transportContext);
            return;
        }

        auto generated = generateIds();
        if (!generated) {
            sendResponse(
                transport,
                jsonRpcError(externalId, -32000, generated.error().message),
                transportContext);
            return;
        }

        std::optional<Domain::ProjectId> projectId;
        const auto suppliedProject = arguments.find("project_id");
        if (suppliedProject != arguments.end() && !suppliedProject->is_string()) {
            sendResponse(
                transport,
                domainFailureResponse(
                    externalId,
                    invalidToolRequest("project_id must be a string.")),
                transportContext);
            return;
        }
        if (suppliedProject != arguments.end()) {
            auto parsedProject = Domain::ProjectId::parse(
                suppliedProject->get_ref<const std::string&>());
            if (!parsedProject) {
                sendResponse(
                    transport,
                    domainFailureResponse(externalId, parsedProject.error()),
                    transportContext);
                return;
            }
            projectId.emplace(std::move(parsedProject).value());
        }

        std::string protocol;
        {
            std::lock_guard lock{stateMutex_};
            protocol = negotiatedProtocol_;
        }
        auto ids = std::move(generated).value();
        Domain::ToolCallRequest request{
            Domain::McpRequestMetadata{
                ids.requestId,
                ids.correlationId,
                clientId,
                std::move(projectId),
                std::move(protocol)},
            name->get<std::string>(),
            arguments.dump()};
        const auto deadline = transportContext.deadline;
        std::stop_source cancellation;
        PendingCall pending{
            externalId,
            key.value(),
            std::move(request),
            descriptor->tool.effect,
            ids.operationId,
            ids.correlationId,
            cancellation,
            deadline,
            transportContext};

        bool duplicate{};
        bool capacityExceeded{};
        {
            std::lock_guard lock{stateMutex_};
            if (requests_.contains(key.value())) {
                duplicate = true;
            } else if (pendingCalls_.size() >=
                       McpServer::MaximumPendingToolCalls) {
                capacityExceeded = true;
            } else {
                requests_.emplace(
                    key.value(),
                    RequestControl{ids.operationId, cancellation});
                pendingCalls_.push_back(std::move(pending));
            }
        }

        if (duplicate) {
            sendResponse(
                transport,
                jsonRpcError(
                    externalId,
                    -32600,
                    "A request with this id is already active."),
                transportContext);
            return;
        }
        if (capacityExceeded) {
            sendResponse(
                transport,
                jsonRpcError(
                    externalId,
                    -32000,
                    "The bounded MCP tool-call queue is full."),
                transportContext);
            return;
        }
        stateChanged_.notify_one();
    }

    [[nodiscard]] Domain::Result<GeneratedIds> generateIds() noexcept
    {
        try {
            auto operation = uuidGenerator_.next();
            auto request = uuidGenerator_.next();
            auto correlation = uuidGenerator_.next();
            if (!operation) {
                return Domain::Result<GeneratedIds>::failure(
                    std::move(operation).error());
            }
            if (!request) {
                return Domain::Result<GeneratedIds>::failure(
                    std::move(request).error());
            }
            if (!correlation) {
                return Domain::Result<GeneratedIds>::failure(
                    std::move(correlation).error());
            }

            auto requestId = Domain::RequestId::parse(request.value().value());
            auto correlationId = Domain::CorrelationId::parse(
                correlation.value().value());
            if (!requestId) {
                return Domain::Result<GeneratedIds>::failure(
                    std::move(requestId).error());
            }
            if (!correlationId) {
                return Domain::Result<GeneratedIds>::failure(
                    std::move(correlationId).error());
            }
            return Domain::Result<GeneratedIds>::success(GeneratedIds{
                Domain::OperationId{std::move(operation).value()},
                std::move(requestId).value(),
                std::move(correlationId).value()});
        } catch (...) {
            return Domain::Result<GeneratedIds>::failure(internalError(
                "The MCP server could not allocate per-call identities."));
        }
    }

    [[nodiscard]] Domain::Result<Json> toolsList() const noexcept
    {
        try {
            Json tools = Json::array();
            tools.get_ref<Json::array_t&>().reserve(catalog_.tools().size());
            for (const auto& descriptor : catalog_.tools()) {
                auto schema = Json::parse(
                    descriptor.inputSchema.begin(),
                    descriptor.inputSchema.end(),
                    nullptr,
                    false,
                    false);
                if (schema.is_discarded() || !schema.is_object()) {
                    return Domain::Result<Json>::failure(internalError(
                        "The MCP catalog contains an invalid input schema."));
                }
                tools.push_back(Json{
                    {"description", descriptor.tool.description},
                    {"inputSchema", std::move(schema)},
                    {"name", descriptor.tool.name}});
            }
            return Domain::Result<Json>::success(std::move(tools));
        } catch (...) {
            return Domain::Result<Json>::failure(internalError(
                "The MCP tool catalog could not be serialized."));
        }
    }

    void workerLoop(
        Contracts::IMcpTransport& transport,
        const std::stop_token workerCancellation) noexcept
    {
        try {
            while (true) {
                std::optional<PendingCall> pending;
                {
                    std::unique_lock lock{stateMutex_};
                    stateChanged_.wait(lock, [&] {
                        return stopping_ || inputClosed_ ||
                            !pendingCalls_.empty() ||
                            workerCancellation.stop_requested();
                    });
                    if (stopping_ || workerCancellation.stop_requested()) {
                        return;
                    }
                    if (pendingCalls_.empty()) {
                        if (inputClosed_) {
                            return;
                        }
                        continue;
                    }
                    pending.emplace(std::move(pendingCalls_.front()));
                    pendingCalls_.pop_front();
                }
                executeToolCall(transport, std::move(pending.value()));
            }
        } catch (...) {
            setTerminalFailure(internalError(
                "The MCP tool-call worker failed at its boundary."));
            requestStop(true);
        }
    }

    void executeToolCall(
        Contracts::IMcpTransport& transport,
        PendingCall pending) noexcept
    {
        try {
            const std::stop_callback parentCancellation{
                pending.transportContext.cancellation,
                [&pending] { pending.cancellation.request_stop(); }};
            Domain::OperationContext context{
                pending.operationId,
                pending.deadline,
                pending.cancellation.get_token(),
                pending.correlationId};

            Json response;
            if (context.isCancellationRequested()) {
                response = jsonRpcError(
                    pending.externalId, -32800, "Cancelled");
            } else if (context.isExpired(clock_.monotonicNow())) {
                response = domainFailureResponse(
                    pending.externalId,
                    Domain::makeError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        "The MCP tool-call deadline expired."));
            } else {
                auto authority = contextResolver_.resolve(
                    pending.request,
                    pending.effect,
                    context);
                if (context.isCancellationRequested()) {
                    response = jsonRpcError(
                        pending.externalId, -32800, "Cancelled");
                } else if (context.isExpired(clock_.monotonicNow())) {
                    response = domainFailureResponse(
                        pending.externalId,
                        Domain::makeError(
                            Domain::ErrorCodes::DeadlineExceeded,
                            "The MCP tool-call deadline expired."));
                } else if (!authority) {
                    response = domainFailureResponse(
                        pending.externalId,
                        authority.error());
                } else {
                    auto outcome = router_.invoke(
                        pending.request,
                        authority.value(),
                        context);
                    if (context.isCancellationRequested()) {
                        response = jsonRpcError(
                            pending.externalId, -32800, "Cancelled");
                    } else if (context.isExpired(clock_.monotonicNow())) {
                        response = domainFailureResponse(
                            pending.externalId,
                            Domain::makeError(
                                Domain::ErrorCodes::DeadlineExceeded,
                                "The MCP tool-call deadline expired."));
                    } else if (!outcome) {
                        response = domainFailureResponse(
                            pending.externalId,
                            outcome.error());
                    } else {
                        response = outcomeResponse(
                            pending.externalId,
                            outcome.value());
                    }
                }
            }

            {
                std::lock_guard lock{stateMutex_};
                requests_.erase(pending.externalKey);
            }
            sendResponse(transport, response, pending.transportContext);
        } catch (...) {
            {
                std::lock_guard lock{stateMutex_};
                requests_.erase(pending.externalKey);
            }
            sendResponse(
                transport,
                jsonRpcError(
                    pending.externalId,
                    -32000,
                    "The MCP tool call failed at its boundary."),
                pending.transportContext);
        }
    }

    [[nodiscard]] Json outcomeResponse(
        const Json& externalId,
        const Domain::ToolCallOutcome& outcome) const
    {
        const McpJsonCodec codec;
        auto canonical = codec.canonicalize(outcome.canonicalPayload);
        if (!canonical) {
            return domainFailureResponse(
                externalId,
                internalError(
                    "The tool router returned a payload outside the MCP limits."));
        }

        const bool isError = !outcome.receipt.ok || outcome.receipt.error.has_value();
        const auto emptyResponse = jsonRpcResult(
            externalId,
            toolEnvelope(Json::object(), isError)).dump();
        constexpr std::size_t EmptyStructuredPayloadBytes = 2U;
        constexpr std::size_t EmptyTextPayloadBytes = 4U;
        constexpr std::size_t EmptyPayloadContributionBytes =
            EmptyStructuredPayloadBytes + EmptyTextPayloadBytes;
        const auto fixedEnvelopeBytes =
            emptyResponse.size() - EmptyPayloadContributionBytes;
        const auto encodedTextBytes =
            encodedJsonStringBytes(canonical.value());
        if (canonical.value().size() >
                McpJsonCodec::MaximumDocumentBytes - fixedEnvelopeBytes ||
            encodedTextBytes >
                McpJsonCodec::MaximumDocumentBytes - fixedEnvelopeBytes -
                    canonical.value().size()) {
            return domainFailureResponse(
                externalId,
                internalError(
                    "The tool result exceeds the MCP response limit."));
        }

        auto payload = Json::parse(
            canonical.value().begin(),
            canonical.value().end(),
            nullptr,
            false,
            false);
        if (payload.is_discarded() || !payload.is_object()) {
            return domainFailureResponse(
                externalId,
                internalError(
                    "The tool router returned an invalid canonical payload."));
        }
        return jsonRpcResult(
            externalId,
            toolEnvelope(std::move(payload), isError));
    }

    void sendResponse(
        Contracts::IMcpTransport& transport,
        const Json& response,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const McpJsonCodec codec;
            auto serialized = codec.canonicalize(response.dump());
            if (!serialized) {
                std::optional<Json> responseId;
                const auto id = response.find("id");
                if (id != response.end() && validRequestId(*id)) {
                    responseId.emplace(*id);
                }
                serialized = codec.canonicalize(jsonRpcError(
                    responseId,
                    -32000,
                    "The MCP response exceeds the configured limit.").dump());
            }
            if (!serialized) {
                setTerminalFailure(internalError(
                    "The bounded MCP failure response could not be serialized."));
                requestStop(true);
                return;
            }
            const Domain::McpFrame frame{std::move(serialized).value()};
            std::lock_guard sendLock{sendMutex_};
            auto sent = transport.send(frame, context);
            if (!sent) {
                setTerminalFailure(std::move(sent).error());
                requestStop(true);
            }
        } catch (...) {
            setTerminalFailure(internalError(
                "The MCP response could not be serialized or sent."));
            requestStop(true);
        }
    }

    void setTerminalFailure(Domain::Error error) noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            if (!terminalError_) {
                terminalError_.emplace(std::move(error));
            }
        } catch (...) {
        }
    }

    void requestStop(const bool cancelRouter) noexcept
    {
        std::vector<Domain::OperationId> operations;
        Contracts::IMcpTransport* transport{};
        try {
            operations.reserve(MaximumPendingToolCalls + 1U);
        } catch (...) {
        }
        try {
            {
                std::lock_guard lock{stateMutex_};
                stopping_ = true;
                inputClosed_ = true;
                transport = transport_;
                for (auto& [key, request] : requests_) {
                    static_cast<void>(key);
                    request.cancellation.request_stop();
                    try {
                        operations.push_back(request.operationId);
                    } catch (...) {
                    }
                }
                pendingCalls_.clear();
            }
        } catch (...) {
        }
        stateChanged_.notify_all();
        if (cancelRouter) {
            for (const auto& operation : operations) {
                router_.cancel(operation);
            }
        }
        if (transport != nullptr) {
            transport->shutdown();
        }
    }

    Contracts::IToolCatalog& catalog_;
    Contracts::IToolRouter& router_;
    Contracts::IMcpExecutionContextResolver& contextResolver_;
    Contracts::IUuidGenerator& uuidGenerator_;
    const Contracts::IClock& clock_;

    std::mutex stateMutex_;
    std::mutex sendMutex_;
    std::condition_variable stateChanged_;
    std::deque<PendingCall> pendingCalls_;
    std::unordered_map<std::string, RequestControl> requests_;
    std::unordered_set<std::string> preCancelled_;
    std::deque<std::string> preCancellationOrder_;
    std::optional<Domain::Error> terminalError_;
    Contracts::IMcpTransport* transport_{};
    std::string negotiatedProtocol_{
        McpProtocol::SupportedVersions.front()};
    bool running_{};
    bool stopping_{};
    bool inputClosed_{};
    bool permanentlyShutdown_{};
};

McpServer::McpServer(
    Contracts::IToolCatalog& catalog,
    Contracts::IToolRouter& router,
    Contracts::IMcpExecutionContextResolver& contextResolver,
    Contracts::IUuidGenerator& uuidGenerator,
    const Contracts::IClock& clock)
    : implementation_{std::make_unique<Implementation>(
          catalog,
          router,
          contextResolver,
          uuidGenerator,
          clock)}
{
}

McpServer::~McpServer() noexcept
{
    shutdown();
    if (implementation_) {
        implementation_->waitForRunCompletion();
    }
}

Domain::Result<void> McpServer::run(
    Contracts::IMcpTransport& transport,
    const Domain::McpRole role,
    const Domain::DeploymentId& deploymentId,
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->run(
        transport,
        role,
        deploymentId,
        clientId,
        context);
}

void McpServer::cancel(const Domain::OperationId& operationId) noexcept
{
    implementation_->cancel(operationId);
}

void McpServer::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Mcp
