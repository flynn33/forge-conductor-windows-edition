#include "ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h"

#include "Detail/UtfConversion.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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

constexpr std::size_t MaximumRequestBytes =
    Domain::MaximumContinuityHandoffEncodedBytes;
constexpr std::size_t MaximumActiveRequests = 256U;
constexpr std::size_t MaximumRememberedCancellations = 256U;
constexpr std::size_t MaximumModelBytes = 256U;
constexpr std::size_t MaximumBasePathBytes = 1024U;
constexpr auto MaximumConfiguredTimeout = 120s;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) noexcept(
        std::is_nothrow_move_constructible_v<Callback>)
        : callback_{std::move(callback)}
    {
    }

    ~ScopeExit() noexcept { callback_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Callback callback_;
};

class AtomicInternetHandle final {
public:
    explicit AtomicInternetHandle(const HINTERNET handle) noexcept
        : handle_{handle}
    {
    }

    ~AtomicInternetHandle() noexcept { close(); }

    AtomicInternetHandle(const AtomicInternetHandle&) = delete;
    AtomicInternetHandle& operator=(const AtomicInternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept
    {
        return handle_.load(std::memory_order_acquire);
    }

    void close() noexcept
    {
        const auto handle = handle_.exchange(nullptr, std::memory_order_acq_rel);
        if (handle != nullptr) {
            static_cast<void>(WinHttpCloseHandle(handle));
        }
    }

private:
    std::atomic<HINTERNET> handle_;
};

struct HttpResponse final {
    DWORD statusCode{};
    std::string body;
    std::optional<std::int64_t> inputTokensHeader;
    std::optional<std::int64_t> outputTokensHeader;
};

[[nodiscard]] bool isAsciiLoopbackHost(const std::string_view host) noexcept
{
    if (host == "127.0.0.1" || host == "::1" || host == "[::1]") {
        return true;
    }
    constexpr std::string_view Localhost = "localhost";
    return host.size() == Localhost.size() &&
           std::equal(
               host.begin(),
               host.end(),
               Localhost.begin(),
               [](const unsigned char left, const unsigned char right) {
                   return static_cast<unsigned char>(std::tolower(left)) ==
                          right;
               });
}

[[nodiscard]] bool isBasePathCharacter(const unsigned char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '/' || value == '-' ||
           value == '_' || value == '.' || value == '~';
}

[[nodiscard]] bool hasReservedPathSegment(const std::string_view path)
{
    std::size_t cursor = 1U;
    while (cursor <= path.size()) {
        const auto separator = path.find('/', cursor);
        const auto end = separator == std::string_view::npos
                             ? path.size()
                             : separator;
        const auto segment = path.substr(cursor, end - cursor);
        if (segment.empty() || segment == "." || segment == "..") {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        cursor = separator + 1U;
    }
    return false;
}

[[nodiscard]] WinHttpLocalModelSessionTransportConfiguration
validatedConfiguration(
    WinHttpLocalModelSessionTransportConfiguration configuration)
{
    if (!isAsciiLoopbackHost(configuration.loopbackHost)) {
        throw std::invalid_argument(
            "The local-model transport host must be an explicit loopback host.");
    }
    if (configuration.loopbackHost == "[::1]") {
        configuration.loopbackHost = "::1";
    }
    if (configuration.port == 0U) {
        throw std::invalid_argument(
            "The local-model transport port must be non-zero.");
    }
    if (configuration.basePath.size() > 1U &&
        configuration.basePath.back() == '/') {
        configuration.basePath.pop_back();
    }
    if (configuration.basePath.empty() ||
        configuration.basePath.front() != '/' ||
        configuration.basePath.size() > MaximumBasePathBytes ||
        !std::all_of(
            configuration.basePath.begin(),
            configuration.basePath.end(),
            [](const unsigned char value) {
                return isBasePathCharacter(value);
            }) ||
        (configuration.basePath != "/" &&
         hasReservedPathSegment(configuration.basePath))) {
        throw std::invalid_argument(
            "The local-model transport base path is not a canonical URL path.");
    }
    const auto timeoutIsValid = [](const std::chrono::milliseconds value) {
        return value > 0ms && value <= MaximumConfiguredTimeout;
    };
    if (!timeoutIsValid(configuration.connectTimeout) ||
        !timeoutIsValid(configuration.sendTimeout) ||
        !timeoutIsValid(configuration.receiveTimeout)) {
        throw std::invalid_argument(
            "The local-model transport timeouts must be within (0, 120s].");
    }
    return configuration;
}

[[nodiscard]] std::wstring requireUtf16(
    const std::string_view value,
    const std::string_view field)
{
    auto converted = Detail::strictUtf8ToUtf16(value);
    if (!converted) {
        throw std::invalid_argument(
            std::string{"The local-model transport "} + std::string{field} +
            " is not valid UTF-8.");
    }
    return std::move(converted).value();
}

[[nodiscard]] bool containsNul(const std::string_view value) noexcept
{
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Domain::Result<Json> parseStrictObject(
    const std::string_view encoded,
    const std::initializer_list<std::string_view> allowedKeys,
    const std::initializer_list<std::string_view> requiredKeys)
{
    if (encoded.empty() || encoded.size() > Domain::MaximumNativeResponseBytes ||
        !Domain::isValidUtf8(encoded)) {
        return failure<Json>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned an invalid JSON payload.");
    }

    try {
        bool duplicateKey = false;
        bool excessiveDepth = false;
        std::vector<std::unordered_set<std::string>> objectKeys;
        const auto callback = [&](
                                  const int depth,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (depth > 32) {
                excessiveDepth = true;
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() || !parsed.is_string() ||
                    !objectKeys.back().insert(parsed.get<std::string>()).second) {
                    duplicateKey = true;
                }
            } else if (event == Json::parse_event_t::object_end &&
                       !objectKeys.empty()) {
                objectKeys.pop_back();
            }
            return true;
        };

        auto document = Json::parse(encoded, callback, true, false);
        if (duplicateKey || excessiveDepth || !document.is_object()) {
            return failure<Json>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned a non-canonical JSON object.");
        }
        for (auto iterator = document.begin(); iterator != document.end();
             ++iterator) {
            if (std::find(
                    allowedKeys.begin(), allowedKeys.end(), iterator.key()) ==
                allowedKeys.end()) {
                return failure<Json>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned an unknown JSON field.");
            }
        }
        for (const auto required : requiredKeys) {
            if (!document.contains(required)) {
                return failure<Json>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider omitted a required JSON field.");
            }
        }
        return Domain::Result<Json>::success(std::move(document));
    } catch (const nlohmann::json::exception&) {
        return failure<Json>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned malformed JSON.");
    } catch (...) {
        return failure<Json>(
            Domain::ErrorCodes::InternalFailure,
            "The local-model response could not be decoded safely.");
    }
}

[[nodiscard]] Domain::Result<std::int64_t> parseUsageValue(
    const Json& value,
    const std::string_view field)
{
    try {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::MalformedMessage,
                std::string{"The local-model provider returned an invalid "} +
                    std::string{field} + ".");
        }
        if (value.is_number_unsigned()) {
            const auto unsignedValue = value.get<std::uint64_t>();
            if (unsignedValue >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                return failure<std::int64_t>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model usage value exceeded its domain bound.");
            }
            return Domain::Result<std::int64_t>::success(
                static_cast<std::int64_t>(unsignedValue));
        }
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model usage value cannot be negative.");
        }
        return Domain::Result<std::int64_t>::success(signedValue);
    } catch (...) {
        return failure<std::int64_t>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model usage value could not be decoded.");
    }
}

[[nodiscard]] Domain::Result<std::optional<std::int64_t>> queryUsageHeader(
    const HINTERNET request,
    const wchar_t* const name)
{
    DWORD bytes = 0U;
    static_cast<void>(WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_CUSTOM,
        name,
        WINHTTP_NO_OUTPUT_BUFFER,
        &bytes,
        WINHTTP_NO_HEADER_INDEX));
    const auto error = GetLastError();
    if (bytes == 0U && error == ERROR_WINHTTP_HEADER_NOT_FOUND) {
        return Domain::Result<std::optional<std::int64_t>>::success(
            std::nullopt);
    }
    if (error != ERROR_INSUFFICIENT_BUFFER ||
        bytes > 128U * sizeof(wchar_t)) {
        return failure<std::optional<std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned an invalid usage header.");
    }

    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1U, L'\0');
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CUSTOM,
            name,
            value.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return failure<std::optional<std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned an unreadable usage header.");
    }
    std::wstring_view text{value.data(), bytes / sizeof(wchar_t)};
    while (!text.empty() && (text.back() == L'\0' || text.back() == L' ' ||
                             text.back() == L'\t')) {
        text.remove_suffix(1U);
    }
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.remove_prefix(1U);
    }
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](const wchar_t character) {
            return character >= L'0' && character <= L'9';
        })) {
        return failure<std::optional<std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned a non-numeric usage header.");
    }
    const auto utf8 = Detail::strictUtf16ToUtf8(text);
    if (!utf8) {
        return failure<std::optional<std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider returned an invalid usage header.");
    }
    std::int64_t parsed{};
    const auto* const first = utf8.value().data();
    const auto* const last = first + utf8.value().size();
    const auto [end, conversionError] = std::from_chars(first, last, parsed);
    if (conversionError != std::errc{} || end != last || parsed < 0) {
        return failure<std::optional<std::int64_t>>(
            Domain::ErrorCodes::MalformedMessage,
            "The local-model provider usage header exceeded its domain bound.");
    }
    return Domain::Result<std::optional<std::int64_t>>::success(parsed);
}

[[nodiscard]] std::vector<std::byte> toBytes(const std::string_view value)
{
    std::vector<std::byte> result(value.size());
    std::transform(value.begin(), value.end(), result.begin(), [](const char byte) {
        return static_cast<std::byte>(static_cast<unsigned char>(byte));
    });
    return result;
}

[[nodiscard]] std::optional<Domain::HostSessionStatus> parseStatus(
    const std::string_view status) noexcept
{
    if (status == "creating") {
        return Domain::HostSessionStatus::Creating;
    }
    if (status == "active") {
        return Domain::HostSessionStatus::Active;
    }
    if (status == "bootstrapping") {
        return Domain::HostSessionStatus::Bootstrapping;
    }
    if (status == "ready") {
        return Domain::HostSessionStatus::Ready;
    }
    if (status == "sealed") {
        return Domain::HostSessionStatus::Sealed;
    }
    if (status == "failed") {
        return Domain::HostSessionStatus::Failed;
    }
    if (status == "cancelled") {
        return Domain::HostSessionStatus::Cancelled;
    }
    return std::nullopt;
}

} // namespace

class WinHttpLocalModelSessionTransport::Impl final {
public:
    explicit Impl(WinHttpLocalModelSessionTransportConfiguration configuration)
        : configuration_{validatedConfiguration(std::move(configuration))},
          host_{requireUtf16(configuration_.loopbackHost, "host")},
          session_{std::make_shared<AtomicInternetHandle>(WinHttpOpen(
              L"Forge Conductor/0.9.0",
              WINHTTP_ACCESS_TYPE_NO_PROXY,
              WINHTTP_NO_PROXY_NAME,
              WINHTTP_NO_PROXY_BYPASS,
              0U))}
    {
        if (session_->get() == nullptr) {
            throw std::runtime_error(
                "The WinHTTP local-model transport could not open a session.");
        }
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> create(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const Json body{
                {"idempotency_key", request.idempotencyKey.value()},
                {"operation_id", request.operationId.value()},
                {"predecessor_session_id", request.predecessorSessionId.value()},
                {"project_id", request.projectId.value()}};
            auto response = performRequest(
                L"POST", sessionsPath(), body.dump(), context);
            if (!response) {
                return failure<Domain::NativeTransportSession>(
                    response.error().code,
                    response.error().message,
                    response.error().retryable);
            }
            auto decoded = parseStrictObject(
                response.value().body,
                {"provider_session_id", "model"},
                {"provider_session_id"});
            if (!decoded ||
                !decoded.value().at("provider_session_id").is_string()) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned an invalid session identity.");
            }
            const auto& providerText =
                decoded.value().at("provider_session_id").get_ref<
                    const std::string&>();
            auto providerId = Domain::ProviderSessionId::parse(providerText, 512U);
            if (!providerId) {
                return failure<Domain::NativeTransportSession>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned an invalid session identity.");
            }

            std::optional<std::string> model;
            if (decoded.value().contains("model") &&
                !decoded.value().at("model").is_null()) {
                if (!decoded.value().at("model").is_string()) {
                    return failure<Domain::NativeTransportSession>(
                        Domain::ErrorCodes::MalformedMessage,
                        "The local-model provider returned an invalid model name.");
                }
                const auto& value = decoded.value().at("model").get_ref<
                    const std::string&>();
                if (value.empty() || value.size() > MaximumModelBytes ||
                    containsNul(value) || !Domain::isValidUtf8(value)) {
                    return failure<Domain::NativeTransportSession>(
                        Domain::ErrorCodes::MalformedMessage,
                        "The local-model provider returned an invalid model name.");
                }
                model = value;
            }
            return Domain::Result<Domain::NativeTransportSession>::success(
                Domain::NativeTransportSession{
                    std::move(providerId).value(), std::move(model)});
        } catch (...) {
            return failure<Domain::NativeTransportSession>(
                Domain::ErrorCodes::InternalFailure,
                "The local-model session creation failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (request.canonicalHandoffUtf8.empty() ||
                request.canonicalHandoffUtf8.size() > MaximumRequestBytes) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The canonical handoff exceeds the local-model request bound.");
            }
            if (!Domain::isValidUtf8(request.canonicalHandoffUtf8) ||
                containsNul(request.canonicalHandoffUtf8)) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The canonical handoff is not valid UTF-8.");
            }
            const Json body{
                {"canonical_handoff", request.canonicalHandoffUtf8},
                {"handoff_id", request.handoffId.value()},
                {"handoff_sha256", request.handoffSha256.value()},
                {"operation_id", request.operationId.value()},
                {"project_id", request.projectId.value()},
                {"provider_session_id", request.providerSessionId.value()},
                {"successor_session_id", request.successorSessionId.value()}};
            const auto encoded = body.dump();
            if (encoded.size() > MaximumRequestBytes) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The local-model bootstrap envelope exceeds its request bound.");
            }
            auto response = performRequest(
                L"POST",
                sessionPath(request.providerSessionId) + "/bootstrap",
                encoded,
                context);
            if (!response) {
                return failure<Domain::NativeBootstrapResponse>(
                    response.error().code,
                    response.error().message,
                    response.error().retryable);
            }
            auto decoded = parseStrictObject(
                response.value().body,
                {"handoff_id",
                 "successor_session_id",
                 "input_tokens",
                 "output_tokens",
                 "usage"},
                {"handoff_id", "successor_session_id"});
            if (!decoded) {
                return failure<Domain::NativeBootstrapResponse>(
                    decoded.error().code,
                    decoded.error().message,
                    decoded.error().retryable);
            }
            const auto& document = decoded.value();
            if (!document.at("handoff_id").is_string() ||
                !document.at("successor_session_id").is_string() ||
                document.at("handoff_id").get<std::string>() !=
                    request.handoffId.value() ||
                document.at("successor_session_id").get<std::string>() !=
                    request.successorSessionId.value()) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The local-model provider acknowledged a different handoff.");
            }

            auto usage = decodeUsage(document, response.value());
            if (!usage) {
                return failure<Domain::NativeBootstrapResponse>(
                    usage.error().code,
                    usage.error().message,
                    usage.error().retryable);
            }
            const Json acknowledgement{
                {"handoff_id", request.handoffId.value()},
                {"successor_session_id", request.successorSessionId.value()}};
            const auto acknowledgementText = acknowledgement.dump();
            if (acknowledgementText.size() >
                Domain::MaximumNativeResponseChunkBytes) {
                return failure<Domain::NativeBootstrapResponse>(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The local-model acknowledgement exceeds its chunk bound.");
            }
            return Domain::Result<Domain::NativeBootstrapResponse>::success(
                Domain::NativeBootstrapResponse{
                    {toBytes(acknowledgementText)},
                    usage.value().first,
                    usage.value().second});
        } catch (...) {
            return failure<Domain::NativeBootstrapResponse>(
                Domain::ErrorCodes::InternalFailure,
                "The local-model bootstrap failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto response =
                performRequest(L"GET", sessionPath(sessionId), {}, context);
            if (!response) {
                return failure<Domain::HostSessionStatus>(
                    response.error().code,
                    response.error().message,
                    response.error().retryable);
            }
            auto decoded = parseStrictObject(
                response.value().body,
                {"provider_session_id", "status"},
                {"status"});
            if (!decoded || !decoded.value().at("status").is_string()) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned an invalid session status.");
            }
            if (decoded.value().contains("provider_session_id")) {
                if (!decoded.value().at("provider_session_id").is_string() ||
                    decoded.value().at("provider_session_id").get<std::string>() !=
                        sessionId.value()) {
                    return failure<Domain::HostSessionStatus>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The local-model provider returned another session's status.");
                }
            }
            const auto status = parseStatus(
                decoded.value().at("status").get_ref<const std::string&>());
            if (!status) {
                return failure<Domain::HostSessionStatus>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned an unknown session status.");
            }
            return Domain::Result<Domain::HostSessionStatus>::success(*status);
        } catch (...) {
            return failure<Domain::HostSessionStatus>(
                Domain::ErrorCodes::InternalFailure,
                "The local-model status query failed safely.");
        }
    }

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId) noexcept
    {
        try {
            std::vector<std::shared_ptr<AtomicInternetHandle>> requests;
            {
                std::lock_guard lock{activityMutex_};
                rememberCancellationLocked(operationId.value());
                const auto found = activeRequests_.find(operationId.value());
                if (found != activeRequests_.end()) {
                    for (const auto& weak : found->second) {
                        if (auto request = weak.lock()) {
                            requests.push_back(std::move(request));
                        }
                    }
                }
            }
            for (const auto& request : requests) {
                request->close();
            }
            if (sessionId &&
                !shutdownRequested_.load(std::memory_order_acquire)) {
                sendCancellation(operationId, *sessionId);
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        if (shutdownRequested_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::vector<std::shared_ptr<AtomicInternetHandle>> requests;
        try {
            std::lock_guard lock{activityMutex_};
            for (const auto& [operation, handles] : activeRequests_) {
                static_cast<void>(operation);
                for (const auto& weak : handles) {
                    if (auto request = weak.lock()) {
                        requests.push_back(std::move(request));
                    }
                }
            }
        } catch (...) {
        }
        for (const auto& request : requests) {
            request->close();
        }
        session_->close();
    }

private:
    enum class RegistrationResult {
        Registered,
        Closed,
        Cancelled,
        LimitExceeded
    };

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const
    {
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The WinHTTP local-model transport is closed."));
        }
        if (context.isCancellationRequested() ||
            isRememberedCancellation(context.operationId.value())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The local-model request was cancelled."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The local-model request deadline has elapsed.",
                true));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool isRememberedCancellation(
        const std::string_view operationId) const
    {
        std::lock_guard lock{activityMutex_};
        return cancelledOperations_.contains(std::string{operationId});
    }

    void rememberCancellationLocked(const std::string& operationId)
    {
        if (cancelledOperations_.contains(operationId)) {
            return;
        }
        while (cancelledOperations_.size() >=
                   MaximumRememberedCancellations &&
               !cancelledOrder_.empty()) {
            cancelledOperations_.erase(cancelledOrder_.front());
            cancelledOrder_.pop_front();
        }
        cancelledOperations_.insert(operationId);
        cancelledOrder_.push_back(operationId);
    }

    [[nodiscard]] RegistrationResult registerRequest(
        const Domain::OperationContext& context,
        const std::shared_ptr<AtomicInternetHandle>& request)
    {
        std::lock_guard lock{activityMutex_};
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return RegistrationResult::Closed;
        }
        if (context.isCancellationRequested() ||
            cancelledOperations_.contains(context.operationId.value())) {
            return RegistrationResult::Cancelled;
        }
        if (activeRequestCount_ >= MaximumActiveRequests) {
            return RegistrationResult::LimitExceeded;
        }
        activeRequests_[context.operationId.value()].push_back(request);
        ++activeRequestCount_;
        return RegistrationResult::Registered;
    }

    void unregisterRequest(
        const std::string& operationId,
        const AtomicInternetHandle* const request) noexcept
    {
        try {
            std::lock_guard lock{activityMutex_};
            const auto found = activeRequests_.find(operationId);
            if (found == activeRequests_.end()) {
                return;
            }
            auto& handles = found->second;
            const auto before = handles.size();
            std::erase_if(handles, [&](const auto& weak) {
                const auto shared = weak.lock();
                return !shared || shared.get() == request;
            });
            const auto removed = before - handles.size();
            activeRequestCount_ = removed > activeRequestCount_
                                      ? 0U
                                      : activeRequestCount_ - removed;
            if (handles.empty()) {
                activeRequests_.erase(found);
            }
        } catch (...) {
        }
    }

    [[nodiscard]] int boundedTimeout(
        const std::chrono::milliseconds configured,
        const Domain::OperationContext& context) const noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= context.deadline) {
            return 1;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - now);
        if (remaining <= 0ms) {
            remaining = 1ms;
        }
        const auto bounded = std::min(configured, remaining);
        return static_cast<int>(std::min<std::int64_t>(
            bounded.count(), static_cast<std::int64_t>(INT_MAX)));
    }

    [[nodiscard]] bool setTimeouts(
        const HINTERNET request,
        const Domain::OperationContext& context) const noexcept
    {
        return WinHttpSetTimeouts(
                   request,
                   boundedTimeout(configuration_.connectTimeout, context),
                   boundedTimeout(configuration_.connectTimeout, context),
                   boundedTimeout(configuration_.sendTimeout, context),
                   boundedTimeout(configuration_.receiveTimeout, context)) !=
               FALSE;
    }

    template <typename T>
    [[nodiscard]] Domain::Result<T> requestFailure(
        const std::string_view stage,
        const DWORD systemError,
        const Domain::OperationContext& context) const
    {
        if (context.isCancellationRequested() ||
            isRememberedCancellation(context.operationId.value()) ||
            systemError == ERROR_WINHTTP_OPERATION_CANCELLED ||
            systemError == ERROR_OPERATION_ABORTED) {
            return failure<T>(
                Domain::ErrorCodes::Cancelled,
                "The local-model request was cancelled during " +
                    std::string{stage} + ".");
        }
        if (context.isExpired(std::chrono::steady_clock::now()) ||
            systemError == ERROR_WINHTTP_TIMEOUT || systemError == ERROR_TIMEOUT) {
            return failure<T>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The local-model request exceeded its deadline during " +
                    std::string{stage} + ".",
                true);
        }
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return failure<T>(
                Domain::ErrorCodes::TransportClosed,
                "The WinHTTP local-model transport closed during " +
                    std::string{stage} + ".",
                true);
        }
        return failure<T>(
            Domain::ErrorCodes::TransportClosed,
            "The local-model provider transport failed during " +
                std::string{stage} + " (Win32 " +
                std::to_string(systemError) + ").",
            true);
    }

    [[nodiscard]] Domain::Result<HttpResponse> performRequest(
        const wchar_t* const method,
        const std::string& path,
        const std::string_view body,
        const Domain::OperationContext& context)
    {
        auto valid = validateContext(context);
        if (!valid) {
            return failure<HttpResponse>(
                valid.error().code,
                valid.error().message,
                valid.error().retryable);
        }
        if (body.size() > MaximumRequestBytes) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::PayloadTooLarge,
                "The local-model request exceeds its 128 KiB bound.");
        }
        const auto widePath = requireUtf16(path, "request path");
        const auto sessionHandle = session_->get();
        if (sessionHandle == nullptr) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::TransportClosed,
                "The WinHTTP local-model transport is closed.");
        }

        const auto connection = std::make_shared<AtomicInternetHandle>(
            WinHttpConnect(
                sessionHandle,
                host_.c_str(),
                configuration_.port,
                0U));
        if (connection->get() == nullptr) {
            return requestFailure<HttpResponse>(
                "connection setup", GetLastError(), context);
        }
        const wchar_t* acceptTypes[]{L"application/json", nullptr};
        const auto flags = configuration_.secure ? WINHTTP_FLAG_SECURE : 0U;
        const auto request = std::make_shared<AtomicInternetHandle>(
            WinHttpOpenRequest(
                connection->get(),
                method,
                widePath.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                acceptTypes,
                flags));
        if (request->get() == nullptr) {
            return requestFailure<HttpResponse>(
                "request setup", GetLastError(), context);
        }

        const auto registration = registerRequest(context, request);
        if (registration != RegistrationResult::Registered) {
            request->close();
            if (registration == RegistrationResult::LimitExceeded) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The local-model transport reached its active-request bound.",
                    true);
            }
            if (registration == RegistrationResult::Cancelled) {
                return failure<HttpResponse>(
                    Domain::ErrorCodes::Cancelled,
                    "The local-model request was cancelled.");
            }
            return failure<HttpResponse>(
                Domain::ErrorCodes::TransportClosed,
                "The WinHTTP local-model transport is closed.");
        }
        const auto operationId = context.operationId.value();
        ScopeExit unregister{[this, operationId, raw = request.get()]() noexcept {
            unregisterRequest(operationId, raw);
        }};
        auto cancellation = [weak = std::weak_ptr<AtomicInternetHandle>{request}]()
                                noexcept {
            if (const auto active = weak.lock()) {
                active->close();
            }
        };
        std::stop_callback stopCallback{context.cancellation, cancellation};

        if (!setTimeouts(request->get(), context)) {
            return requestFailure<HttpResponse>(
                "timeout configuration", GetLastError(), context);
        }
        constexpr wchar_t Headers[] =
            L"Accept: application/json\r\n"
            L"Content-Type: application/json; charset=utf-8\r\n";
        void* requestBody = body.empty()
                                ? WINHTTP_NO_REQUEST_DATA
                                : const_cast<char*>(body.data());
        if (!WinHttpSendRequest(
                request->get(),
                Headers,
                static_cast<DWORD>(-1L),
                requestBody,
                static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()),
                0U)) {
            return requestFailure<HttpResponse>(
                "request send", GetLastError(), context);
        }
        valid = validateContext(context);
        if (!valid) {
            return failure<HttpResponse>(
                valid.error().code,
                valid.error().message,
                valid.error().retryable);
        }
        if (!setTimeouts(request->get(), context) ||
            !WinHttpReceiveResponse(request->get(), nullptr)) {
            return requestFailure<HttpResponse>(
                "response receive", GetLastError(), context);
        }

        HttpResponse response;
        DWORD statusBytes = sizeof(response.statusCode);
        if (!WinHttpQueryHeaders(
                request->get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &response.statusCode,
                &statusBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            return requestFailure<HttpResponse>(
                "status decode", GetLastError(), context);
        }
        auto inputHeader =
            queryUsageHeader(request->get(), L"X-Forge-Input-Tokens");
        auto outputHeader =
            queryUsageHeader(request->get(), L"X-Forge-Output-Tokens");
        if (!inputHeader || !outputHeader) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned invalid usage headers.");
        }
        response.inputTokensHeader = std::move(inputHeader).value();
        response.outputTokensHeader = std::move(outputHeader).value();

        std::size_t chunks = 0U;
        while (true) {
            valid = validateContext(context);
            if (!valid) {
                return failure<HttpResponse>(
                    valid.error().code,
                    valid.error().message,
                    valid.error().retryable);
            }
            if (!setTimeouts(request->get(), context)) {
                return requestFailure<HttpResponse>(
                    "response timeout update", GetLastError(), context);
            }
            DWORD available = 0U;
            if (!WinHttpQueryDataAvailable(request->get(), &available)) {
                return requestFailure<HttpResponse>(
                    "response availability", GetLastError(), context);
            }
            if (available == 0U) {
                break;
            }
            while (available > 0U) {
                if (chunks >= Domain::MaximumNativeResponseChunks ||
                    response.body.size() >= Domain::MaximumNativeResponseBytes) {
                    return failure<HttpResponse>(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The local-model response exceeds its bounded capacity.");
                }
                const auto room =
                    Domain::MaximumNativeResponseBytes - response.body.size();
                const auto requested = static_cast<DWORD>(std::min<std::size_t>(
                    {static_cast<std::size_t>(available),
                     Domain::MaximumNativeResponseChunkBytes,
                     room}));
                if (requested == 0U) {
                    return failure<HttpResponse>(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The local-model response exceeds its bounded capacity.");
                }
                std::vector<char> buffer(requested);
                DWORD received = 0U;
                if (!WinHttpReadData(
                        request->get(), buffer.data(), requested, &received)) {
                    return requestFailure<HttpResponse>(
                        "response body", GetLastError(), context);
                }
                if (received == 0U) {
                    available = 0U;
                    break;
                }
                response.body.append(buffer.data(), received);
                available -= std::min(available, received);
                ++chunks;
            }
        }
        valid = validateContext(context);
        if (!valid) {
            return failure<HttpResponse>(
                valid.error().code,
                valid.error().message,
                valid.error().retryable);
        }
        if (response.statusCode == 429U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::RateLimited,
                "The local-model provider rate limited the request.",
                true);
        }
        if (response.statusCode == 404U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::SessionNotFound,
                "The local-model provider session was not found.");
        }
        if (response.statusCode == 408U || response.statusCode == 504U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The local-model provider timed out the request.",
                true);
        }
        if (response.statusCode == 401U || response.statusCode == 403U) {
            return failure<HttpResponse>(
                Domain::ErrorCodes::Unauthorized,
                "The local-model provider rejected the request authority.");
        }
        if (response.statusCode < 200U || response.statusCode >= 300U) {
            const auto retryable = response.statusCode >= 500U;
            return failure<HttpResponse>(
                retryable ? Domain::ErrorCodes::TransportClosed
                          : Domain::ErrorCodes::InvalidRequest,
                "The local-model provider rejected the request with HTTP " +
                    std::to_string(response.statusCode) + ".",
                retryable);
        }
        return Domain::Result<HttpResponse>::success(std::move(response));
    }

    [[nodiscard]] Domain::Result<std::pair<std::int64_t, std::int64_t>>
    decodeUsage(const Json& document, const HttpResponse& response) const
    {
        const auto hasFlatInput = document.contains("input_tokens");
        const auto hasFlatOutput = document.contains("output_tokens");
        if (hasFlatInput != hasFlatOutput) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned incomplete JSON usage.");
        }
        const auto hasUsage = document.contains("usage");
        if (hasUsage && hasFlatInput) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned ambiguous JSON usage.");
        }

        std::optional<std::pair<std::int64_t, std::int64_t>> jsonUsage;
        if (hasFlatInput) {
            auto input = parseUsageValue(document.at("input_tokens"), "input_tokens");
            auto output =
                parseUsageValue(document.at("output_tokens"), "output_tokens");
            if (!input || !output) {
                return failure<std::pair<std::int64_t, std::int64_t>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned invalid JSON usage.");
            }
            jsonUsage = std::pair{input.value(), output.value()};
        } else if (hasUsage) {
            const auto& usage = document.at("usage");
            if (!usage.is_object() || usage.size() != 2U ||
                !usage.contains("input_tokens") ||
                !usage.contains("output_tokens")) {
                return failure<std::pair<std::int64_t, std::int64_t>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned invalid nested usage.");
            }
            auto input = parseUsageValue(usage.at("input_tokens"), "input_tokens");
            auto output = parseUsageValue(
                usage.at("output_tokens"), "output_tokens");
            if (!input || !output) {
                return failure<std::pair<std::int64_t, std::int64_t>>(
                    Domain::ErrorCodes::MalformedMessage,
                    "The local-model provider returned invalid nested usage.");
            }
            jsonUsage = std::pair{input.value(), output.value()};
        }

        if (response.inputTokensHeader.has_value() !=
            response.outputTokensHeader.has_value()) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned incomplete usage headers.");
        }
        std::optional<std::pair<std::int64_t, std::int64_t>> headerUsage;
        if (response.inputTokensHeader) {
            headerUsage = std::pair{
                *response.inputTokensHeader, *response.outputTokensHeader};
        }
        if (jsonUsage && headerUsage && jsonUsage != headerUsage) {
            return failure<std::pair<std::int64_t, std::int64_t>>(
                Domain::ErrorCodes::MalformedMessage,
                "The local-model provider returned conflicting usage values.");
        }
        return Domain::Result<std::pair<std::int64_t, std::int64_t>>::success(
            headerUsage.value_or(jsonUsage.value_or(std::pair{0LL, 0LL})));
    }

    [[nodiscard]] std::string sessionsPath() const
    {
        return configuration_.basePath == "/"
                   ? "/sessions"
                   : configuration_.basePath + "/sessions";
    }

    [[nodiscard]] std::string sessionPath(
        const Domain::ProviderSessionId& sessionId) const
    {
        return sessionsPath() + "/" + sessionId.value();
    }

    void sendCancellation(
        const Domain::OperationId& operationId,
        const Domain::ProviderSessionId& sessionId) noexcept
    {
        try {
            const auto sessionHandle = session_->get();
            if (sessionHandle == nullptr) {
                return;
            }
            const auto path =
                requireUtf16(sessionPath(sessionId) + "/cancel", "cancel path");
            AtomicInternetHandle connection{WinHttpConnect(
                sessionHandle, host_.c_str(), configuration_.port, 0U)};
            if (connection.get() == nullptr) {
                return;
            }
            const wchar_t* acceptTypes[]{L"application/json", nullptr};
            AtomicInternetHandle request{WinHttpOpenRequest(
                connection.get(),
                L"POST",
                path.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                acceptTypes,
                configuration_.secure ? WINHTTP_FLAG_SECURE : 0U)};
            if (request.get() == nullptr) {
                return;
            }
            const auto timeout = static_cast<int>(std::min<std::int64_t>(
                {configuration_.connectTimeout.count(),
                 configuration_.sendTimeout.count(),
                 configuration_.receiveTimeout.count(),
                 2'000LL}));
            if (!WinHttpSetTimeouts(
                    request.get(), timeout, timeout, timeout, timeout)) {
                return;
            }
            const auto body = Json{{"operation_id", operationId.value()}}.dump();
            constexpr wchar_t Headers[] =
                L"Accept: application/json\r\n"
                L"Content-Type: application/json; charset=utf-8\r\n";
            if (!WinHttpSendRequest(
                    request.get(),
                    Headers,
                    static_cast<DWORD>(-1L),
                    const_cast<char*>(body.data()),
                    static_cast<DWORD>(body.size()),
                    static_cast<DWORD>(body.size()),
                    0U)) {
                return;
            }
            static_cast<void>(WinHttpReceiveResponse(request.get(), nullptr));
        } catch (...) {
        }
    }

    const WinHttpLocalModelSessionTransportConfiguration configuration_;
    const std::wstring host_;
    std::shared_ptr<AtomicInternetHandle> session_;
    std::atomic_bool shutdownRequested_{};
    mutable std::mutex activityMutex_;
    std::unordered_map<
        std::string,
        std::vector<std::weak_ptr<AtomicInternetHandle>>>
        activeRequests_;
    std::size_t activeRequestCount_{};
    std::unordered_set<std::string> cancelledOperations_;
    std::deque<std::string> cancelledOrder_;
};

WinHttpLocalModelSessionTransport::WinHttpLocalModelSessionTransport(
    WinHttpLocalModelSessionTransportConfiguration configuration)
    : implementation_{std::make_unique<Impl>(std::move(configuration))}
{
}

WinHttpLocalModelSessionTransport::~WinHttpLocalModelSessionTransport() noexcept
{
    shutdown();
}

Domain::Result<Domain::NativeTransportSession>
WinHttpLocalModelSessionTransport::createSession(
    const Domain::SessionCreationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->create(request, context);
    } catch (...) {
        return failure<Domain::NativeTransportSession>(
            Domain::ErrorCodes::InternalFailure,
            "The WinHTTP local-model transport failed safely.");
    }
}

Domain::Result<Domain::NativeBootstrapResponse>
WinHttpLocalModelSessionTransport::bootstrap(
    const Domain::NativeBootstrapRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->bootstrap(request, context);
    } catch (...) {
        return failure<Domain::NativeBootstrapResponse>(
            Domain::ErrorCodes::InternalFailure,
            "The WinHTTP local-model transport failed safely.");
    }
}

Domain::Result<Domain::HostSessionStatus>
WinHttpLocalModelSessionTransport::query(
    const Domain::ProviderSessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->query(sessionId, context);
    } catch (...) {
        return failure<Domain::HostSessionStatus>(
            Domain::ErrorCodes::InternalFailure,
            "The WinHTTP local-model transport failed safely.");
    }
}

void WinHttpLocalModelSessionTransport::cancel(
    const Domain::OperationId& operationId,
    const std::optional<Domain::ProviderSessionId>& sessionId) noexcept
{
    try {
        implementation_->cancel(operationId, sessionId);
    } catch (...) {
    }
}

void WinHttpLocalModelSessionTransport::shutdown() noexcept
{
    try {
        if (implementation_) {
            implementation_->shutdown();
        }
    } catch (...) {
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
