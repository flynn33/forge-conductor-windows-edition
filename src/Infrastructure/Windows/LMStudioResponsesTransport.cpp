#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"

#include "Detail/UtfConversion.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::size_t MaximumHttpBodyBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumRequestBytes =
    Domain::MaximumContinuityHandoffEncodedBytes + 64U * 1024U;
constexpr std::size_t MaximumRememberedCancellations = 256U;
constexpr auto MaximumTimeout = 5min;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

class InternetHandle final {
public:
    explicit InternetHandle(const HINTERNET value = nullptr) noexcept
        : value_{value}
    {
    }

    ~InternetHandle() noexcept { close(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept
    {
        return value_.load(std::memory_order_acquire);
    }

    void close() noexcept
    {
        const auto value = value_.exchange(nullptr, std::memory_order_acq_rel);
        if (value != nullptr) {
            static_cast<void>(WinHttpCloseHandle(value));
        }
    }

private:
    std::atomic<HINTERNET> value_;
};

struct HttpResponse final {
    DWORD status{};
    std::string body;
};

struct FunctionCall final {
    std::string callId;
    Json arguments;
};

[[nodiscard]] bool isLoopback(const std::string_view host) noexcept
{
    if (host == "127.0.0.1" || host == "::1" || host == "[::1]") {
        return true;
    }
    constexpr std::string_view Localhost = "localhost";
    return host.size() == Localhost.size() &&
        std::equal(
            host.begin(), host.end(), Localhost.begin(),
            [](const unsigned char left, const unsigned char right) {
                return static_cast<unsigned char>(std::tolower(left)) == right;
            });
}

[[nodiscard]] bool validPath(const std::string_view path) noexcept
{
    if (path.empty() || path.front() != '/' || path.size() > 512U ||
        (path.size() > 1U && path.back() == '/')) {
        return false;
    }
    return std::all_of(path.begin(), path.end(), [](const unsigned char value) {
        return std::isalnum(value) != 0 || value == '/' || value == '-' ||
            value == '_' || value == '.' || value == '~';
    }) && path.find("..") == std::string_view::npos;
}

[[nodiscard]] LMStudioResponsesTransportConfiguration validateConfiguration(
    LMStudioResponsesTransportConfiguration value)
{
    if (!isLoopback(value.loopbackHost)) {
        throw std::invalid_argument(
            "LM Studio Responses transport requires an explicit loopback host.");
    }
    if (value.loopbackHost == "[::1]") {
        value.loopbackHost = "::1";
    }
    if (value.port == 0U || !validPath(value.basePath)) {
        throw std::invalid_argument(
            "LM Studio Responses endpoint configuration is invalid.");
    }
    const auto timeoutValid = [](const std::chrono::milliseconds timeout) {
        return timeout > 0ms && timeout <= MaximumTimeout;
    };
    if (!timeoutValid(value.connectTimeout) || !timeoutValid(value.sendTimeout) ||
        !timeoutValid(value.receiveTimeout)) {
        throw std::invalid_argument(
            "LM Studio Responses timeouts must be within (0, 5 minutes].");
    }
    const auto textValid = [](const std::string& text, const std::size_t maximum) {
        return !text.empty() && text.size() <= maximum &&
            text.find('\0') == std::string::npos && Domain::isValidUtf8(text);
    };
    if (value.model && !textValid(*value.model, 256U)) {
        throw std::invalid_argument("The configured LM Studio model is invalid.");
    }
    if (value.bearerToken && !textValid(*value.bearerToken, 4096U)) {
        throw std::invalid_argument("The configured LM Studio bearer token is invalid.");
    }
    return value;
}

[[nodiscard]] Domain::Result<Json> parseObject(const std::string_view body)
{
    if (body.empty() || body.size() > MaximumHttpBodyBytes ||
        body.find('\0') != std::string_view::npos || !Domain::isValidUtf8(body)) {
        return failure<Json>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned an invalid JSON response body.");
    }
    try {
        auto value = Json::parse(body);
        if (!value.is_object()) {
            return failure<Json>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio returned a non-object Responses payload.");
        }
        return Domain::Result<Json>::success(std::move(value));
    } catch (const nlohmann::json::exception&) {
        return failure<Json>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned malformed JSON.");
    }
}

[[nodiscard]] Domain::Result<std::string> responseId(const Json& response)
{
    try {
        if (!response.contains("id") || !response.at("id").is_string()) {
            return failure<std::string>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio omitted the Responses id.");
        }
        const auto id = response.at("id").get<std::string>();
        auto parsed = Domain::ProviderSessionId::parse(id, 512U);
        if (!parsed) {
            return failure<std::string>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio returned an invalid Responses id.");
        }
        return Domain::Result<std::string>::success(id);
    } catch (...) {
        return failure<std::string>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned an unreadable Responses id.");
    }
}

[[nodiscard]] Domain::Result<FunctionCall> contextGetCall(const Json& response)
{
    try {
        if (!response.contains("output") || !response.at("output").is_array()) {
            return failure<FunctionCall>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio did not return a Responses output array.");
        }
        std::optional<FunctionCall> found;
        for (const auto& item : response.at("output")) {
            if (!item.is_object() || item.value("type", "") != "function_call") {
                continue;
            }
            if (found || item.value("name", "") != "context_get" ||
                !item.contains("call_id") || !item.at("call_id").is_string() ||
                !item.contains("arguments")) {
                return failure<FunctionCall>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio returned an unexpected bootstrap function call.");
            }
            Json arguments;
            if (item.at("arguments").is_string()) {
                arguments = Json::parse(item.at("arguments").get<std::string>());
            } else if (item.at("arguments").is_object()) {
                arguments = item.at("arguments");
            } else {
                return failure<FunctionCall>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio returned invalid context_get arguments.");
            }
            const auto callId = item.at("call_id").get<std::string>();
            if (callId.empty() || callId.size() > 512U ||
                callId.find('\0') != std::string::npos) {
                return failure<FunctionCall>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio returned an invalid function call id.");
            }
            found = FunctionCall{callId, std::move(arguments)};
        }
        if (!found) {
            return failure<FunctionCall>(
                Domain::ErrorCodes::IntegrityFailure,
                "The live successor did not request context_get.");
        }
        return Domain::Result<FunctionCall>::success(std::move(*found));
    } catch (const nlohmann::json::exception&) {
        return failure<FunctionCall>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned malformed function-call arguments.");
    }
}

[[nodiscard]] Domain::Result<std::vector<Domain::ManagedFunctionCall>>
managedFunctionCalls(const Json& response)
{
    try {
        if (!response.contains("output") && response.contains("output_text") &&
            response.at("output_text").is_string()) {
            return Domain::Result<
                std::vector<Domain::ManagedFunctionCall>>::success({});
        }
        if (!response.contains("output") || !response.at("output").is_array()) {
            return failure<std::vector<Domain::ManagedFunctionCall>>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio did not return a Responses output array.");
        }
        std::vector<Domain::ManagedFunctionCall> calls;
        for (const auto& item : response.at("output")) {
            if (!item.is_object() || item.value("type", "") != "function_call") {
                continue;
            }
            if (!item.contains("call_id") || !item.at("call_id").is_string() ||
                !item.contains("name") || !item.at("name").is_string() ||
                !item.contains("arguments")) {
                return failure<std::vector<Domain::ManagedFunctionCall>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio returned an incomplete managed function call.");
            }
            const auto callId = item.at("call_id").get<std::string>();
            const auto name = item.at("name").get<std::string>();
            if (callId.empty() || callId.size() > 512U || name.empty() ||
                name.size() > 128U || callId.find('\0') != std::string::npos ||
                name.find('\0') != std::string::npos) {
                return failure<std::vector<Domain::ManagedFunctionCall>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio returned an invalid managed function identity.");
            }
            Json arguments;
            if (item.at("arguments").is_string()) {
                arguments = Json::parse(item.at("arguments").get<std::string>());
            } else {
                arguments = item.at("arguments");
            }
            if (!arguments.is_object()) {
                return failure<std::vector<Domain::ManagedFunctionCall>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio returned non-object managed function arguments.");
            }
            const auto duplicate = std::find_if(
                calls.begin(),
                calls.end(),
                [&](const auto& call) { return call.callId == callId; });
            if (duplicate != calls.end()) {
                return failure<std::vector<Domain::ManagedFunctionCall>>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio repeated a managed function call id.");
            }
            calls.push_back({callId, name, arguments.dump()});
        }
        return Domain::Result<std::vector<Domain::ManagedFunctionCall>>::success(
            std::move(calls));
    } catch (const nlohmann::json::exception&) {
        return failure<std::vector<Domain::ManagedFunctionCall>>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned malformed managed function arguments.");
    }
}

[[nodiscard]] Domain::Result<std::string> outputText(const Json& response)
{
    try {
        if (response.contains("output_text") &&
            response.at("output_text").is_string()) {
            return Domain::Result<std::string>::success(
                response.at("output_text").get<std::string>());
        }
        if (response.contains("output") && response.at("output").is_array()) {
            std::string result;
            for (const auto& item : response.at("output")) {
                if (!item.is_object() || item.value("type", "") != "message" ||
                    !item.contains("content") || !item.at("content").is_array()) {
                    continue;
                }
                for (const auto& content : item.at("content")) {
                    if (content.is_object() &&
                        content.value("type", "") == "output_text" &&
                        content.contains("text") && content.at("text").is_string()) {
                        result += content.at("text").get<std::string>();
                    }
                }
            }
            if (!result.empty()) {
                return Domain::Result<std::string>::success(std::move(result));
            }
        }
        return failure<std::string>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned no successor acknowledgement text.");
    } catch (...) {
        return failure<std::string>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio returned unreadable successor acknowledgement text.");
    }
}

[[nodiscard]] Domain::Result<std::pair<std::int64_t, std::int64_t>> usage(
    const Json& response)
{
    try {
        if (!response.contains("usage") || response.at("usage").is_null()) {
            return Domain::Result<std::pair<std::int64_t, std::int64_t>>::success(
                {0, 0});
        }
        const auto& value = response.at("usage");
        if (!value.is_object() || !value.contains("input_tokens") ||
            !value.contains("output_tokens") ||
            !value.at("input_tokens").is_number_unsigned() ||
            !value.at("output_tokens").is_number_unsigned()) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio returned invalid Responses usage.");
        }
        const auto input = value.at("input_tokens").get<std::uint64_t>();
        const auto output = value.at("output_tokens").get<std::uint64_t>();
        constexpr auto Maximum = static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());
        if (input > Maximum || output > Maximum) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio Responses usage exceeded its numeric bound.");
        }
        return Domain::Result<std::pair<std::int64_t, std::int64_t>>::success(
            {static_cast<std::int64_t>(input),
             static_cast<std::int64_t>(output)});
    } catch (...) {
        return failure<std::pair<std::int64_t, std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "LM Studio Responses usage could not be decoded.");
    }
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        result.push_back(static_cast<std::byte>(byte));
    }
    return result;
}

} // namespace

class LMStudioResponsesTransport::Impl final {
public:
    explicit Impl(LMStudioResponsesTransportConfiguration configuration)
        : configuration_{validateConfiguration(std::move(configuration))},
          host_{Detail::strictUtf8ToUtf16(configuration_.loopbackHost).value()},
          session_{std::make_shared<InternetHandle>(WinHttpOpen(
              L"Forge Conductor LM Studio Responses/1.1.7",
              WINHTTP_ACCESS_TYPE_NO_PROXY,
              WINHTTP_NO_PROXY_NAME,
              WINHTTP_NO_PROXY_BYPASS,
              0U))}
    {
        if (session_->get() == nullptr) {
            throw std::runtime_error("WinHTTP could not open the LM Studio session.");
        }
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> create(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto selected = discoverModel(context);
            if (!selected) {
                return failure<Domain::NativeTransportSession>(
                    selected.error().code,
                    selected.error().message,
                    selected.error().retryable);
            }
            auto pending = Domain::ProviderSessionId::parse(
                "forge-pending-" + request.idempotencyKey.value(), 512U);
            if (!pending) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::InvalidRequest,
                    "The native session idempotency key cannot form a provider binding.");
            }
            return Domain::Result<Domain::NativeTransportSession>::success(
                {std::move(pending).value(), std::move(selected).value()});
        } catch (...) {
            return failure<Domain::NativeTransportSession>(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio model discovery failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (request.canonicalHandoffUtf8.empty() ||
                request.canonicalHandoffUtf8.size() >
                    Domain::MaximumContinuityHandoffEncodedBytes ||
                request.canonicalHandoffUtf8.find('\0') != std::string::npos ||
                !Domain::isValidUtf8(request.canonicalHandoffUtf8)) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The canonical handoff is invalid or exceeds its bound.");
            }
            auto selected = discoverModel(context);
            if (!selected) {
                return failure<Domain::NativeBootstrapResponse>(
                    selected.error().code,
                    selected.error().message,
                    selected.error().retryable);
            }

            const std::string acknowledgement =
                "After context_get succeeds, respond with only this JSON object: "
                "{\"handoff_id\":\"" + request.handoffId.value() +
                "\",\"successor_session_id\":\"" +
                request.successorSessionId.value() + "\"}.";
            const Json firstBody{
                {"model", selected.value()},
                {"input",
                 Json::array({Json{
                     {"role", "user"},
                     {"content",
                      "You are the fresh successor for Forge Conductor project " +
                          request.projectId.value() + ". Call context_get exactly "
                          "once with handoff_id " + request.handoffId.value() +
                          ". Verify the returned canonical handoff digest is " +
                          request.handoffSha256.value() + ". " + acknowledgement}}})},
                {"tools",
                 Json::array({Json{
                     {"type", "function"},
                     {"name", "context_get"},
                     {"description", "Retrieve the canonical Forge continuity handoff."},
                     {"parameters",
                      Json{
                          {"type", "object"},
                          {"additionalProperties", false},
                          {"properties",
                           Json{{"handoff_id", Json{{"type", "string"}}}}},
                          {"required", Json::array({"handoff_id"})}}}}})},
                {"tool_choice", "required"},
                {"parallel_tool_calls", false},
                {"store", true}};
            auto first = postResponses(firstBody, context);
            if (!first) {
                return failure<Domain::NativeBootstrapResponse>(
                    first.error().code, first.error().message,
                    first.error().retryable);
            }
            auto firstId = responseId(first.value());
            auto call = contextGetCall(first.value());
            auto firstUsage = usage(first.value());
            if (!firstId || !call || !firstUsage) {
                const auto& error = !firstId ? firstId.error()
                    : !call ? call.error() : firstUsage.error();
                return failure<Domain::NativeBootstrapResponse>(
                    error.code, error.message, error.retryable);
            }
            if (!call.value().arguments.is_object() ||
                call.value().arguments.size() != 1U ||
                call.value().arguments.value("handoff_id", "") !=
                    request.handoffId.value()) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The live successor requested a different continuity handoff.");
            }

            const Json secondBody{
                {"model", selected.value()},
                {"previous_response_id", firstId.value()},
                {"input",
                 Json::array({Json{
                     {"type", "function_call_output"},
                     {"call_id", call.value().callId},
                     {"output", request.canonicalHandoffUtf8}}})},
                {"store", true}};
            auto second = postResponses(secondBody, context);
            if (!second) {
                return failure<Domain::NativeBootstrapResponse>(
                    second.error().code, second.error().message,
                    second.error().retryable);
            }
            auto secondId = responseId(second.value());
            auto text = outputText(second.value());
            auto secondUsage = usage(second.value());
            if (!secondId || !text || !secondUsage) {
                const auto& error = !secondId ? secondId.error()
                    : !text ? text.error() : secondUsage.error();
                return failure<Domain::NativeBootstrapResponse>(
                    error.code, error.message, error.retryable);
            }
            if (text.value().empty() ||
                text.value().size() > Domain::MaximumNativeResponseChunkBytes ||
                !Domain::isValidUtf8(text.value()) ||
                text.value().find('\0') != std::string::npos) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The live successor acknowledgement is invalid.");
            }
            auto providerId = Domain::ProviderSessionId::parse(
                secondId.value(), 512U);
            if (!providerId) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The terminal LM Studio response id is invalid.");
            }
            const auto add = [](const std::int64_t left,
                                const std::int64_t right) {
                const auto maximum = (std::numeric_limits<std::int64_t>::max)();
                return left > maximum - right ? maximum : left + right;
            };
            {
                std::lock_guard lock{stateMutex_};
                readyResponses_.insert(secondId.value());
            }
            return Domain::Result<Domain::NativeBootstrapResponse>::success(
                {{bytes(text.value())},
                 add(firstUsage.value().first, secondUsage.value().first),
                 add(firstUsage.value().second, secondUsage.value().second),
                 std::move(providerId).value()});
        } catch (...) {
            return failure<Domain::NativeBootstrapResponse>(
                Domain::ErrorCodes::InternalFailure,
                "The LM Studio Responses bootstrap failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& id,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (id.value().starts_with("forge-pending-")) {
                return Domain::Result<Domain::HostSessionStatus>::success(
                    Domain::HostSessionStatus::Active);
            }
            {
                std::lock_guard lock{stateMutex_};
                if (readyResponses_.contains(id.value())) {
                    return Domain::Result<Domain::HostSessionStatus>::success(
                        Domain::HostSessionStatus::Ready);
                }
            }
            auto response = perform(
                L"GET", responsePath(id.value()), {}, context);
            if (!response) {
                return failure<Domain::HostSessionStatus>(
                    response.error().code, response.error().message,
                    response.error().retryable);
            }
            auto document = parseObject(response.value().body);
            if (!document) {
                return failure<Domain::HostSessionStatus>(
                    document.error().code, document.error().message,
                    document.error().retryable);
            }
            if (document.value().value("id", "") != id.value()) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio returned another Responses record.");
            }
            const auto status = document.value().value("status", "completed");
            if (status == "completed") {
                return Domain::Result<Domain::HostSessionStatus>::success(
                    Domain::HostSessionStatus::Ready);
            }
            if (status == "failed" || status == "cancelled" ||
                status == "incomplete") {
                return Domain::Result<Domain::HostSessionStatus>::success(
                    Domain::HostSessionStatus::Failed);
            }
            return Domain::Result<Domain::HostSessionStatus>::success(
                Domain::HostSessionStatus::Bootstrapping);
        } catch (...) {
            return failure<Domain::HostSessionStatus>(
                Domain::ErrorCodes::InternalFailure,
                "The LM Studio Responses status query failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedProviderTurnResult> complete(
        const Domain::ManagedProviderTurnRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (request.authorityGeneration == 0U ||
                (request.input.empty() == request.toolOutputs.empty()) ||
                request.input.size() > Domain::MaximumManagedRunTaskBytes ||
                request.input.find('\0') != std::string::npos ||
                !Domain::isValidUtf8(request.input)) {
                return failure<Domain::ManagedProviderTurnResult>(
                    Domain::ErrorCodes::InvalidRequest,
                    "The ordinary LM Studio request is invalid.");
            }
            auto selected = discoverModel(context);
            if (!selected) {
                return failure<Domain::ManagedProviderTurnResult>(
                    selected.error().code,
                    selected.error().message,
                    selected.error().retryable);
            }
            Json body{{"model", selected.value()}, {"store", true}};
            if (!request.input.empty()) {
                body["input"] = request.input;
            } else {
                body["input"] = Json::array();
                for (const auto& output : request.toolOutputs) {
                    if (output.callId.empty() || output.callId.size() > 512U ||
                        output.canonicalOutput.empty() ||
                        output.canonicalOutput.size() > MaximumHttpBodyBytes ||
                        !Domain::isValidUtf8(output.canonicalOutput)) {
                        return failure<Domain::ManagedProviderTurnResult>(
                            Domain::ErrorCodes::InvalidRequest,
                            "The managed function output is invalid.");
                    }
                    body["input"].push_back(Json{
                        {"type", "function_call_output"},
                        {"call_id", output.callId},
                        {"output", output.canonicalOutput}});
                }
            }
            if (!request.tools.empty()) {
                body["tools"] = Json::array();
                for (const auto& descriptor : request.tools) {
                    Json parameters = Json::parse(descriptor.inputSchema);
                    if (!parameters.is_object()) {
                        return failure<Domain::ManagedProviderTurnResult>(
                            Domain::ErrorCodes::InvalidRequest,
                            "A managed tool schema is not a JSON object.");
                    }
                    body["tools"].push_back(Json{
                        {"type", "function"},
                        {"name", descriptor.tool.name},
                        {"description", descriptor.tool.description},
                        {"parameters", std::move(parameters)}});
                }
                body["parallel_tool_calls"] = false;
            }
            if (request.previousResponseId) {
                body["previous_response_id"] =
                    request.previousResponseId->value();
            }
            auto response = postResponses(body, context);
            if (!response) {
                return failure<Domain::ManagedProviderTurnResult>(
                    response.error().code,
                    response.error().message,
                    response.error().retryable);
            }
            auto id = responseId(response.value());
            auto tokenUsage = usage(response.value());
            auto calls = managedFunctionCalls(response.value());
            if (!id || !tokenUsage || !calls) {
                const auto& error = !id ? id.error()
                    : !tokenUsage ? tokenUsage.error() : calls.error();
                return failure<Domain::ManagedProviderTurnResult>(
                    error.code, error.message, error.retryable);
            }
            auto text = outputText(response.value());
            if (!text && calls.value().empty()) {
                return failure<Domain::ManagedProviderTurnResult>(
                    text.error().code, text.error().message,
                    text.error().retryable);
            }
            std::string responseText = text
                ? std::move(text).value()
                : std::string{};
            if (responseText.size() > Domain::MaximumManagedRunOutputBytes ||
                responseText.find('\0') != std::string::npos ||
                !Domain::isValidUtf8(responseText)) {
                return failure<Domain::ManagedProviderTurnResult>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The ordinary LM Studio response text is invalid.");
            }
            auto providerId = Domain::ProviderSessionId::parse(
                id.value(), 512U);
            if (!providerId) {
                return failure<Domain::ManagedProviderTurnResult>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The ordinary LM Studio response id is invalid.");
            }
            const auto input = static_cast<std::uint64_t>(
                tokenUsage.value().first);
            const auto output = static_cast<std::uint64_t>(
                tokenUsage.value().second);
            const auto maximum =
                (std::numeric_limits<std::uint64_t>::max)();
            const auto retained =
                input > maximum - output ? maximum : input + output;
            {
                std::lock_guard lock{stateMutex_};
                readyResponses_.insert(id.value());
            }
            return Domain::Result<
                Domain::ManagedProviderTurnResult>::success(
                {std::move(providerId).value(),
                 std::move(responseText),
                 input,
                 output,
                 retained,
                 std::move(calls).value()});
        } catch (...) {
            return failure<Domain::ManagedProviderTurnResult>(
                Domain::ErrorCodes::InternalFailure,
                "The ordinary LM Studio request failed safely.");
        }
    }

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& providerId) noexcept
    {
        try {
            std::shared_ptr<InternetHandle> active;
            {
                std::lock_guard lock{stateMutex_};
                rememberCancelled(operationId.value());
                const auto found = active_.find(operationId.value());
                if (found != active_.end()) {
                    active = found->second.lock();
                }
            }
            if (active) {
                active->close();
            }
            if (providerId && !providerId->value().starts_with("forge-pending-")) {
                const Json body{{"response_id", providerId->value()}};
                const Domain::OperationContext cancellationContext{
                    operationId,
                    std::chrono::steady_clock::now() + 2s,
                    {},
                    Domain::CorrelationId::parse(
                        "lm-studio-response-cancel").value()};
                static_cast<void>(perform(
                    L"POST", responsePath(providerId->value()) + "/cancel",
                    body.dump(), cancellationContext, false));
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            if (stopping_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            std::vector<std::shared_ptr<InternetHandle>> handles;
            {
                std::lock_guard lock{stateMutex_};
                for (auto& [operation, weak] : active_) {
                    (void)operation;
                    if (auto handle = weak.lock()) {
                        handles.push_back(std::move(handle));
                    }
                }
                active_.clear();
            }
            for (const auto& handle : handles) {
                handle->close();
            }
            session_->close();
        } catch (...) {
        }
    }

private:
    [[nodiscard]] Domain::Result<std::string> discoverModel(
        const Domain::OperationContext& context)
    {
        {
            std::lock_guard lock{stateMutex_};
            if (selectedModel_) {
                return Domain::Result<std::string>::success(*selectedModel_);
            }
        }
        auto response = perform(L"GET", modelsPath(), {}, context);
        if (!response) {
            return failure<std::string>(
                response.error().code, response.error().message,
                response.error().retryable);
        }
        auto document = parseObject(response.value().body);
        if (!document) {
            return failure<std::string>(
                document.error().code, document.error().message,
                document.error().retryable);
        }
        try {
            if (!document.value().contains("data") ||
                !document.value().at("data").is_array()) {
                return failure<std::string>(
                    Domain::ErrorCodes::MalformedMessage,
                    "LM Studio /models returned no model collection.");
            }
            std::vector<std::string> discovered;
            for (const auto& item : document.value().at("data")) {
                if (item.is_object() && item.contains("id") &&
                    item.at("id").is_string()) {
                    const auto id = item.at("id").get<std::string>();
                    if (!id.empty() && id.size() <= 256U &&
                        id.find('\0') == std::string::npos &&
                        Domain::isValidUtf8(id)) {
                        discovered.push_back(id);
                    }
                }
            }
            std::string selected;
            if (configuration_.model) {
                const auto match = std::find(
                    discovered.begin(), discovered.end(),
                    *configuration_.model);
                if (match == discovered.end()) {
                    return failure<std::string>(
                        Domain::ErrorCodes::SessionNotFound,
                        "The configured LM Studio model is not loaded.");
                }
                selected = *match;
            } else if (!discovered.empty()) {
                selected = discovered.front();
            } else {
                return failure<std::string>(
                    Domain::ErrorCodes::SessionNotFound,
                    "LM Studio has no loaded model for Responses.");
            }
            {
                std::lock_guard lock{stateMutex_};
                if (!selectedModel_) {
                    selectedModel_ = selected;
                }
                selected = *selectedModel_;
            }
            return Domain::Result<std::string>::success(std::move(selected));
        } catch (const nlohmann::json::exception&) {
            return failure<std::string>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio /models returned malformed model metadata.");
        }
    }

    [[nodiscard]] Domain::Result<Json> postResponses(
        const Json& body,
        const Domain::OperationContext& context)
    {
        const auto encoded = body.dump();
        if (encoded.size() > MaximumRequestBytes) {
            return failure<Json>(
                Domain::ErrorCodes::PayloadTooLarge,
                "The LM Studio Responses request exceeds its bound.");
        }
        auto response = perform(L"POST", responsesPath(), encoded, context);
        if (!response) {
            return failure<Json>(
                response.error().code, response.error().message,
                response.error().retryable);
        }
        return parseObject(response.value().body);
    }

    [[nodiscard]] std::string modelsPath() const
    {
        return configuration_.basePath + "/models";
    }

    [[nodiscard]] std::string responsesPath() const
    {
        return configuration_.basePath + "/responses";
    }

    [[nodiscard]] std::string responsePath(const std::string_view id) const
    {
        return responsesPath() + "/" + std::string{id};
    }

    void rememberCancelled(const std::string& operation)
    {
        if (cancelled_.insert(operation).second) {
            cancelledOrder_.push_back(operation);
        }
        while (cancelledOrder_.size() > MaximumRememberedCancellations) {
            cancelled_.erase(cancelledOrder_.front());
            cancelledOrder_.pop_front();
        }
    }

    [[nodiscard]] bool wasCancelled(const std::string& operation) const
    {
        std::lock_guard lock{stateMutex_};
        return cancelled_.contains(operation);
    }

    [[nodiscard]] Domain::Result<HttpResponse> perform(
        const wchar_t* method,
        const std::string& path,
        const std::string& body,
        const Domain::OperationContext& context,
        const bool track = true)
    {
        if (stopping_.load(std::memory_order_acquire)) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::TransportClosed,
                "The LM Studio Responses transport is closed.");
        }
        if (context.isCancellationRequested() ||
            wasCancelled(context.operationId.value())) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::Cancelled,
                "The LM Studio Responses request was cancelled.");
        }
        const auto now = std::chrono::steady_clock::now();
        if (context.deadline <= now) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The LM Studio Responses request deadline elapsed.", true);
        }
        const auto widePath = Detail::strictUtf8ToUtf16(path);
        if (!widePath) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::InvalidRequest,
                "The LM Studio request path is invalid.");
        }
        InternetHandle connection{WinHttpConnect(
            session_->get(), host_.c_str(), configuration_.port, 0U)};
        if (connection.get() == nullptr) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::InternalFailure,
                "Could not connect to the LM Studio local server.", true);
        }
        const wchar_t* accept[]{L"application/json", nullptr};
        auto request = std::make_shared<InternetHandle>(WinHttpOpenRequest(
            connection.get(), method, widePath.value().c_str(), nullptr,
            WINHTTP_NO_REFERER, accept,
            configuration_.secure ? WINHTTP_FLAG_SECURE : 0U));
        if (request->get() == nullptr) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::InternalFailure,
                "Could not create the LM Studio HTTP request.", true);
        }
        if (track) {
            std::lock_guard lock{stateMutex_};
            if (active_.contains(context.operationId.value())) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::OwnershipConflict,
                    "An LM Studio request already owns this operation id.");
            }
            active_.emplace(context.operationId.value(), request);
        }
        const auto untrack = [this, &context, track]() noexcept {
            if (track) {
                try {
                    std::lock_guard lock{stateMutex_};
                    active_.erase(context.operationId.value());
                } catch (...) {
                }
            }
        };
        struct Untrack final {
            decltype(untrack)& action;
            ~Untrack() noexcept { action(); }
        } cleanup{untrack};
        std::stop_callback stop{context.cancellation, [request]() noexcept {
            request->close();
        }};

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - now);
        const auto bounded = [remaining](const std::chrono::milliseconds configured) {
            return static_cast<int>(std::max<std::int64_t>(
                1LL, std::min(configured, remaining).count()));
        };
        if (!WinHttpSetTimeouts(
                request->get(), bounded(configuration_.connectTimeout),
                bounded(configuration_.connectTimeout),
                bounded(configuration_.sendTimeout),
                bounded(configuration_.receiveTimeout))) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::InternalFailure,
                "Could not configure LM Studio request timeouts.", true);
        }
        std::wstring headers =
            L"Accept: application/json\r\nContent-Type: application/json\r\n";
        if (configuration_.bearerToken) {
            auto token = Detail::strictUtf8ToUtf16(*configuration_.bearerToken);
            if (!token) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::InvalidRequest,
                    "The LM Studio bearer token is invalid.");
            }
            headers += L"Authorization: Bearer " + token.value() + L"\r\n";
        }
        void* payload = body.empty()
            ? WINHTTP_NO_REQUEST_DATA
            : const_cast<char*>(body.data());
        if (!WinHttpSendRequest(
                request->get(), headers.c_str(), static_cast<DWORD>(-1L),
                payload, static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), 0U) ||
            !WinHttpReceiveResponse(request->get(), nullptr)) {
            if (context.isCancellationRequested() ||
                wasCancelled(context.operationId.value())) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::Cancelled,
                    "The LM Studio Responses request was cancelled.");
            }
            if (std::chrono::steady_clock::now() >= context.deadline) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The LM Studio Responses request exceeded its deadline.", true);
            }
            return failure<HttpResponse>(
                Domain::ErrorCodes::InternalFailure,
                "The LM Studio Responses HTTP exchange failed.", true);
        }
        DWORD status{};
        DWORD statusBytes = sizeof(status);
        if (!WinHttpQueryHeaders(
                request->get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio returned no HTTP status.");
        }
        std::string responseBody;
        for (;;) {
            DWORD available{};
            if (!WinHttpQueryDataAvailable(request->get(), &available)) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::InternalFailure,
                    "Could not read the LM Studio response.", true);
            }
            if (available == 0U) {
                break;
            }
            if (responseBody.size() > MaximumHttpBodyBytes - available) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The LM Studio response exceeds its bound.");
            }
            std::vector<char> buffer(available);
            DWORD read{};
            if (!WinHttpReadData(
                    request->get(), buffer.data(), available, &read)) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::InternalFailure,
                    "Could not read the LM Studio response body.", true);
            }
            responseBody.append(buffer.data(), read);
        }
        if (status >= 200U && status < 300U) {
            return Domain::Result<HttpResponse>::success(
                {status, std::move(responseBody)});
        }
        if (status == 404U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::SessionNotFound,
                "LM Studio could not find the requested Responses record.");
        }
        if (status == 401U || status == 403U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::Unauthorized,
                "LM Studio rejected the configured local bearer token.");
        }
        if (status == 408U || status == 504U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::DeadlineExceeded,
                "LM Studio timed out the Responses request.", true);
        }
        if (status == 429U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::RateLimited,
                "LM Studio rate limited the Responses request.", true);
        }
        return failure<HttpResponse>(
            Domain::ErrorCodes::InvalidRequest,
            "LM Studio rejected the Responses request with HTTP " +
                std::to_string(status) + ".",
            status >= 500U);
    }

    const LMStudioResponsesTransportConfiguration configuration_;
    const std::wstring host_;
    std::shared_ptr<InternetHandle> session_;
    std::atomic_bool stopping_{};
    mutable std::mutex stateMutex_;
    std::unordered_map<std::string, std::weak_ptr<InternetHandle>> active_;
    std::unordered_set<std::string> cancelled_;
    std::deque<std::string> cancelledOrder_;
    std::unordered_set<std::string> readyResponses_;
    std::optional<std::string> selectedModel_;
};

LMStudioResponsesTransport::LMStudioResponsesTransport(
    LMStudioResponsesTransportConfiguration configuration)
    : implementation_{std::make_unique<Impl>(std::move(configuration))}
{
}

LMStudioResponsesTransport::~LMStudioResponsesTransport() noexcept = default;

Domain::Result<Domain::NativeTransportSession>
LMStudioResponsesTransport::createSession(
    const Domain::SessionCreationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->create(request, context);
}

Domain::Result<Domain::NativeBootstrapResponse>
LMStudioResponsesTransport::bootstrap(
    const Domain::NativeBootstrapRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->bootstrap(request, context);
}

Domain::Result<Domain::HostSessionStatus> LMStudioResponsesTransport::query(
    const Domain::ProviderSessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->query(sessionId, context);
}

Domain::Result<Domain::ManagedProviderTurnResult>
LMStudioResponsesTransport::complete(
    const Domain::ManagedProviderTurnRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->complete(request, context);
}

void LMStudioResponsesTransport::cancel(
    const Domain::OperationId& operationId,
    const std::optional<Domain::ProviderSessionId>& sessionId) noexcept
{
    implementation_->cancel(operationId, sessionId);
}

void LMStudioResponsesTransport::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Infrastructure::Windows
