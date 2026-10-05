#include "ForgeConductor/Infrastructure/Windows/WindowsGitHubReadService.h"
#include "Detail/UtfConversion.h"

#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cctype>
#include <stop_token>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MaximumResponseBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumProjectedResponseBytes = 192U * 1024U;

template <typename T>
[[nodiscard]] Domain::Result<T> fail(const std::string_view code, std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(Domain::makeError(code, std::move(message), retryable));
}

[[nodiscard]] bool repositoryPart(const std::string_view value)
{
    return !value.empty() && value.size() <= 100U && value != "." && value != ".." &&
        std::all_of(value.begin(), value.end(), [](const unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}

[[nodiscard]] std::string escaped(const std::string_view value)
{
    constexpr char Hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(Hex[c >> 4U]);
            result.push_back(Hex[c & 15U]);
        }
    }
    return result;
}

class Internet final {
public:
    explicit Internet(HINTERNET handle) : handle_{handle} {}
    ~Internet() { close(); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    HINTERNET get() const noexcept { return handle_.load(); }
    void close() noexcept { if (const auto value = handle_.exchange(nullptr)) WinHttpCloseHandle(value); }
private:
    std::atomic<HINTERNET> handle_{};
};

[[nodiscard]] Domain::Result<void> check(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::Cancelled, "GitHub inspection was cancelled."));
    if (context.isExpired(std::chrono::steady_clock::now())) return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::DeadlineExceeded, "GitHub inspection exceeded its deadline.", true));
    return Domain::Result<void>::success();
}

[[nodiscard]] std::string paginationHeader(HINTERNET request)
{
    DWORD bytes{};
    static_cast<void>(WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Link",
        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX));
    if (bytes == 0U || bytes > 16U * 1024U) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Link", value.data(),
        &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    auto utf8 = Detail::strictUtf16ToUtf8(value);
    return utf8 ? std::move(utf8).value() : std::string{};
}
} // namespace

WindowsGitHubReadService::WindowsGitHubReadService(std::optional<std::string> bearerToken)
    : bearerToken_{std::move(bearerToken)} {}

std::optional<std::string> WindowsGitHubReadService::configuredEnvironmentToken() noexcept
{
    try {
        for (const wchar_t* name : {L"GH_TOKEN", L"GITHUB_TOKEN"}) {
            const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0U);
            if (needed == 0U || needed > 4097U) continue;
            std::wstring value(needed, L'\0');
            const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
            if (written == 0U || written >= needed) continue;
            value.resize(written);
            auto token = Detail::strictUtf16ToUtf8(value);
            if (token && !token.value().empty()) return std::move(token).value();
        }
        return std::nullopt;
    } catch (...) { return std::nullopt; }
}

Domain::Result<std::string> WindowsGitHubReadService::requestPath(
    const Contracts::GitHubReadRequest& request) noexcept
{
    try {
        const auto slash = request.repository.find('/');
        if (slash == std::string::npos || request.repository.find('/', slash + 1U) != std::string::npos ||
            !repositoryPart(std::string_view{request.repository}.substr(0U, slash)) ||
            !repositoryPart(std::string_view{request.repository}.substr(slash + 1U)) ||
            request.repository.ends_with(".git")) {
            return fail<std::string>(Domain::ErrorCodes::InvalidRequest,
                "repository must be an explicit owner/name without a URL or .git suffix.");
        }
        if (request.page == 0U || request.page > 10000U || request.perPage == 0U || request.perPage > 100U ||
            (request.id && *request.id == 0U)) {
            return fail<std::string>(Domain::ErrorCodes::InvalidRequest,
                "GitHub inspection page, per_page, or id is outside its supported bound.");
        }
        std::string path = "/repos/" + request.repository;
        bool paged{};
        const auto identifier = request.id ? std::to_string(*request.id) : std::string{};
        if (request.operation == "runs") { path += "/actions/runs"; paged = true; }
        else if (request.operation == "run" && request.id) path += "/actions/runs/" + identifier;
        else if (request.operation == "artifacts") {
            path += request.id ? "/actions/runs/" + identifier + "/artifacts" : "/actions/artifacts";
            paged = true;
        }
        else if (request.operation == "artifact" && request.id) path += "/actions/artifacts/" + identifier;
        else if (request.operation == "pull_requests") { path += "/pulls?state=all"; paged = true; }
        else if (request.operation == "pull_request" && request.id) path += "/pulls/" + identifier;
        else if (request.operation == "pull_request_files" && request.id) {
            path += "/pulls/" + identifier + "/files"; paged = true;
        }
        else if (request.operation == "refs" && request.ref && !request.ref->empty() && request.ref->size() <= 256U &&
            ((request.ref->starts_with("heads/") && request.ref->size() > 6U) ||
                (request.ref->starts_with("tags/") && request.ref->size() > 5U)) &&
            request.ref->find_first_of("\\\r\n\0", 0U, 4U) == std::string::npos &&
            request.ref->find("..") == std::string::npos) path += "/git/ref/" + escaped(*request.ref);
        else return fail<std::string>(Domain::ErrorCodes::InvalidRequest,
            "Unsupported GitHub read operation or missing id/ref. refs requires heads/name or tags/name.");
        if (request.ref && request.operation != "refs") return fail<std::string>(
            Domain::ErrorCodes::InvalidRequest, "ref is supported only by the refs operation.");
        if (request.id && (request.operation == "runs" || request.operation == "pull_requests" || request.operation == "refs"))
            return fail<std::string>(Domain::ErrorCodes::InvalidRequest, "This GitHub operation does not accept id.");
        if (paged) path += (path.find('?') == std::string::npos ? "?" : "&") +
            std::string{"per_page="} + std::to_string(request.perPage) + "&page=" + std::to_string(request.page);
        return Domain::Result<std::string>::success(std::move(path));
    } catch (...) {
        return fail<std::string>(Domain::ErrorCodes::InternalFailure, "GitHub inspection request could not be validated.");
    }
}

Domain::Result<std::string> WindowsGitHubReadService::read(
    const Contracts::GitHubReadRequest& request, const Domain::OperationContext& context) noexcept
{
    try {
        auto active = check(context);
        if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
        auto path = requestPath(request);
        if (!path) return path;
        if (bearerToken_ && (bearerToken_->empty() || bearerToken_->size() > 4096U ||
            bearerToken_->find_first_of("\r\n\0", 0U, 3U) != std::string::npos)) {
            return fail<std::string>(Domain::ErrorCodes::InvalidRequest, "Configured GitHub token is invalid.");
        }
        auto widePath = Detail::strictUtf8ToUtf16(path.value());
        if (!widePath) return fail<std::string>(Domain::ErrorCodes::InvalidRequest, "GitHub inspection path is invalid.");
        Internet session{WinHttpOpen(L"ForgeConductor GitHub read-only", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0U)};
        Internet connection{session.get() ? WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0U) : nullptr};
        Internet operation{connection.get() ? WinHttpOpenRequest(connection.get(), L"GET", widePath.value().c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
        if (!operation.get()) return fail<std::string>(Domain::ErrorCodes::HostCapabilityUnavailable,
            "Windows could not create the GitHub inspection connection.", true);
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!WinHttpSetOption(operation.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
            !WinHttpSetOption(operation.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
            return fail<std::string>(Domain::ErrorCodes::HostCapabilityUnavailable, "Windows could not constrain GitHub redirects/authentication.");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - std::chrono::steady_clock::now()).count();
        const auto timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 15000));
        if (!WinHttpSetTimeouts(operation.get(), timeout, timeout, timeout, timeout))
            return fail<std::string>(Domain::ErrorCodes::HostCapabilityUnavailable, "GitHub inspection timeout could not be configured.");
        std::wstring headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (bearerToken_) {
            auto token = Detail::strictUtf8ToUtf16(*bearerToken_);
            if (!token) return fail<std::string>(Domain::ErrorCodes::InvalidRequest, "Configured GitHub token is invalid.");
            headers += L"Authorization: Bearer " + token.value() + L"\r\n";
        }
        std::stop_callback cancel{context.cancellation, [&operation]() noexcept { operation.close(); }};
        // Per-stage WinHTTP timeouts do not establish an absolute operation
        // deadline. Close the request at that deadline, just as cancellation does.
        std::jthread deadlineGuard{[&operation, deadline = context.deadline](std::stop_token stop) noexcept {
            try {
                std::mutex mutex;
                std::condition_variable_any changed;
                std::unique_lock lock{mutex};
                static_cast<void>(changed.wait_until(lock, stop, deadline, [] { return false; }));
                if (!stop.stop_requested()) operation.close();
            } catch (...) { operation.close(); }
        }};
        if (!WinHttpSendRequest(operation.get(), headers.c_str(), static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0U, 0U, 0U) || !WinHttpReceiveResponse(operation.get(), nullptr)) {
            const auto error = GetLastError();
            active = check(context);
            if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
            return fail<std::string>(error == ERROR_WINHTTP_TIMEOUT ? Domain::ErrorCodes::DeadlineExceeded :
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "GitHub inspection connection failed (WinHTTP " + std::to_string(error) + ").", true);
        }
        active = check(context);
        if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
        DWORD status{}, bytes{sizeof(status)};
        if (!WinHttpQueryHeaders(operation.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
            return fail<std::string>(Domain::ErrorCodes::MalformedMessage, "GitHub returned no HTTP status.");
        DWORD rateRemaining{}, remainingBytes{sizeof(rateRemaining)};
        const bool hasRemaining = WinHttpQueryHeaders(operation.get(), WINHTTP_QUERY_CUSTOM | WINHTTP_QUERY_FLAG_NUMBER,
            L"X-RateLimit-Remaining", &rateRemaining, &remainingBytes, WINHTTP_NO_HEADER_INDEX) != FALSE;
        DWORD retryAfter{}, retryBytes{sizeof(retryAfter)};
        const bool hasRetryAfter = WinHttpQueryHeaders(operation.get(), WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &retryAfter, &retryBytes, WINHTTP_NO_HEADER_INDEX) != FALSE;
        const bool rateLimited = status == 429U || (status == 403U && ((hasRemaining && rateRemaining == 0U) || hasRetryAfter));
        if (status != 200U) return fail<std::string>(rateLimited ? Domain::ErrorCodes::RateLimited :
            status == 401U || status == 403U ? Domain::ErrorCodes::Unauthorized :
            status == 404U ? Domain::ErrorCodes::RecordNotFound : Domain::ErrorCodes::InvalidRequest,
            "GitHub inspection returned HTTP " + std::to_string(status) +
            (rateLimited ? "; the GitHub rate limit was reached; wait before retrying." :
                status == 404U ? "; the resource is unavailable or private to this credential." : ".") +
            (rateLimited && hasRetryAfter ? " Retry-After seconds: " + std::to_string(retryAfter) + "." : ""),
            rateLimited || status >= 500U);
        const auto link = paginationHeader(operation.get());
        std::string body;
        for (;;) {
            active = check(context);
            if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
            std::array<char, 8192> buffer{};
            DWORD read{};
            if (!WinHttpReadData(operation.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                const auto error = GetLastError();
                active = check(context);
                if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
                return fail<std::string>(error == ERROR_WINHTTP_TIMEOUT ? Domain::ErrorCodes::DeadlineExceeded :
                    Domain::ErrorCodes::HostCapabilityUnavailable, "GitHub inspection response could not be read.", true);
            }
            if (read == 0U) break;
            if (read > MaximumResponseBytes - body.size()) return fail<std::string>(Domain::ErrorCodes::PayloadTooLarge,
                "GitHub inspection response exceeds 2 MiB; request a smaller per_page.");
            body.append(buffer.data(), read);
        }
        active = check(context);
        if (!active) return Domain::Result<std::string>::failure(std::move(active).error());
        return projectResponse(request, body, bearerToken_.has_value(),
            link.find("rel=\"next\"") != std::string::npos);
    } catch (const nlohmann::json::exception&) {
        return fail<std::string>(Domain::ErrorCodes::MalformedMessage, "GitHub inspection returned malformed JSON.");
    } catch (...) {
        return fail<std::string>(Domain::ErrorCodes::InternalFailure, "GitHub inspection failed safely.");
    }
}

Domain::Result<std::string> WindowsGitHubReadService::projectResponse(
    const Contracts::GitHubReadRequest& request, const std::string_view body,
    const bool authenticated, const bool hasNextPage) noexcept
{
    try {
        auto path = requestPath(request);
        if (!path) return path;
        if (body.size() > MaximumResponseBytes) return fail<std::string>(Domain::ErrorCodes::PayloadTooLarge,
            "GitHub inspection response exceeds 2 MiB; request a smaller per_page.");
        auto data = Json::parse(body);
        if (!data.is_object() && !data.is_array()) return fail<std::string>(Domain::ErrorCodes::MalformedMessage,
            "GitHub inspection returned unexpected JSON.");
        Json result{{"ok", true}, {"repository", request.repository}, {"operation", request.operation},
            {"source_url", "https://api.github.com" + path.value()}, {"http_method", "GET"}, {"http_status", 200},
            {"authenticated", authenticated}, {"page", request.page}, {"per_page", request.perPage},
            {"has_next_page", hasNextPage}, {"next_page", hasNextPage ? Json(request.page + 1U) : Json(nullptr)},
            {"observed_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}, {"data", std::move(data)}};
        if (result.at("data").is_object() && result.at("data").contains("total_count"))
            result["total_count"] = result.at("data").at("total_count");
        auto serialized = result.dump();
        if (serialized.size() > MaximumProjectedResponseBytes) return fail<std::string>(Domain::ErrorCodes::PayloadTooLarge,
            "GitHub inspection page exceeds the MCP-safe 192 KiB payload bound; reduce per_page to 10 or less and retry.");
        return Domain::Result<std::string>::success(std::move(serialized));
    } catch (const nlohmann::json::exception&) {
        return fail<std::string>(Domain::ErrorCodes::MalformedMessage, "GitHub inspection returned malformed JSON.");
    } catch (...) {
        return fail<std::string>(Domain::ErrorCodes::InternalFailure, "GitHub inspection response could not be projected.");
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
