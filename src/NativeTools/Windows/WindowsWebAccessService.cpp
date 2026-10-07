#include "ForgeConductor/NativeTools/Windows/WindowsWebAccessService.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "ForgeConductor/Domain/Utf8.h"
#include <Windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ForgeConductor::NativeTools::Windows {
namespace {
using Json = nlohmann::json;
namespace Utf = Infrastructure::Windows::Detail;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message, bool retryable = false) {
    throw Failure{Domain::makeError(code, std::move(message), retryable)};
}
template<class T> T take(Domain::Result<T> value) {
    if (!value) throw Failure{value.error()};
    return std::move(value).value();
}
void check(const Domain::OperationContext& context, Domain::MonotonicTimePoint deadline) {
    if (context.isCancellationRequested()) fail(Domain::ErrorCodes::Cancelled, "Web request was cancelled.");
    if (context.isExpired(std::chrono::steady_clock::now()) || std::chrono::steady_clock::now() >= deadline)
        fail(Domain::ErrorCodes::DeadlineExceeded, "Web request exceeded its total time budget.", true);
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string text(const Json& args, const char* key, std::size_t maximum, bool required = true) {
    if (!args.contains(key)) {
        if (required) fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is required.");
        return {};
    }
    if (!args[key].is_string()) fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be a string.");
    auto value = args[key].get<std::string>();
    if ((required && value.empty()) || value.size() > maximum || value.find('\0') != std::string::npos || !Domain::isValidUtf8(value))
        fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is empty, too long, or invalid UTF-8.");
    return value;
}
std::size_t number(const Json& args, const char* key, std::size_t fallback, std::size_t maximum) {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_number_integer() || args[key] < 1 || args[key] > maximum)
        fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be an integer within its supported range.");
    return args[key].get<std::size_t>();
}
struct Url final {
    std::wstring wide, host, path;
    INTERNET_PORT port{};
    bool secure{};
    std::string encoded;
};
Url parseUrl(std::string value) {
    if (value.empty() || value.size() > 8192U || !Domain::isValidUtf8(value) ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32U || c == 127U || c == '\\'; }))
        fail(Domain::ErrorCodes::InvalidRequest, "URL must be a bounded HTTP(S) URL without whitespace, controls, or backslashes.");
    const auto fragment = value.find('#');
    if (fragment != std::string::npos) value.resize(fragment);
    Url result;
    result.encoded = std::move(value);
    result.wide = take(Utf::strictUtf8ToUtf16(result.encoded));
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1L);
    if (!WinHttpCrackUrl(result.wide.c_str(), static_cast<DWORD>(result.wide.size()), 0U, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength || !parts.nPort)
        fail(Domain::ErrorCodes::InvalidRequest, "URL must use HTTP(S), have a host, and contain no embedded credentials.");
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (result.path.empty()) result.path = L"/";
    if (parts.dwExtraInfoLength) result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    result.port = parts.nPort;
    result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return result;
}
Url redirect(const Url& base, const std::string& target) {
    const auto wide = take(Utf::strictUtf8ToUtf16(target));
    std::wstring combined(16384U, L'\0');
    DWORD size = static_cast<DWORD>(combined.size());
    if (FAILED(UrlCombineW(base.wide.c_str(), wide.c_str(), combined.data(), &size, URL_DONT_ESCAPE_EXTRA_INFO)))
        fail(Domain::ErrorCodes::InvalidRequest, "Response redirect could not be resolved.");
    combined.resize(size);
    auto result = parseUrl(take(Utf::strictUtf16ToUtf8(combined)));
    if (base.secure && !result.secure)
        fail(Domain::ErrorCodes::Unauthorized, "HTTPS to HTTP redirects are refused.");
    return result;
}
bool sameOrigin(const Url& a, const Url& b) {
    return a.secure == b.secure && a.port == b.port && lower(take(Utf::strictUtf16ToUtf8(a.host))) == lower(take(Utf::strictUtf16ToUtf8(b.host)));
}
std::wstring requestHeaders(const Json& args) {
    if (!args.contains("headers")) return {};
    if (!args["headers"].is_object() || args["headers"].size() > 32U)
        fail(Domain::ErrorCodes::InvalidRequest, "headers must be an object with at most 32 entries.");
    const std::set<std::string> managed{"host", "content-length", "transfer-encoding", "connection", "proxy-authorization", "proxy-connection", "cookie", "set-cookie", "accept-encoding"};
    std::string encoded;
    std::set<std::string> seen;
    for (auto it = args["headers"].begin(); it != args["headers"].end(); ++it) {
        const auto name = it.key();
        const auto normalized = lower(name);
        if (name.empty() || name.size() > 128U || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view{"!#$%&'*+-.^_`|~"}.find(static_cast<char>(c)) != std::string_view::npos;
            }) || managed.contains(normalized) || !seen.insert(normalized).second || !it.value().is_string())
            fail(Domain::ErrorCodes::InvalidRequest, "Header name is invalid, duplicated, or managed by the transport.");
        const auto value = it.value().get<std::string>();
        if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return (c < 32U && c != '\t') || c > 126U; }))
            fail(Domain::ErrorCodes::InvalidRequest, "Header values must be ASCII without forbidden controls.");
        encoded += name + ": " + value + "\r\n";
        if (encoded.size() > 16U * 1024U) fail(Domain::ErrorCodes::PayloadTooLarge, "Headers exceeded 16 KiB.");
    }
    return take(Utf::strictUtf8ToUtf16(encoded));
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

    [[nodiscard]] HINTERNET release() noexcept
    {
        return value_.exchange(nullptr, std::memory_order_acq_rel);
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

// The public transport waits for each async operation. Cancellation may close
// a pending request only after its submitting API has returned. Keep callback
// context and all WinHTTP buffers alive until the final HANDLE_CLOSING notice.
class AsyncRequest final {
public:
    explicit AsyncRequest(const HINTERNET value, std::string body)
        : value_{value}, body_{std::move(body)}
    {
        if (!value_) { initializationError_ = GetLastError(); return; }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(value_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
            initializationError_ = GetLastError(); return;
        }
        if (WinHttpSetStatusCallback(value_, callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0U) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
            initializationError_ = GetLastError(); return;
        }
        callbackInstalled_ = true;
    }

    ~AsyncRequest() noexcept
    {
        close();
        if (callbackInstalled_) {
            std::unique_lock lock{stateMutex_};
            changed_.wait(lock, [&] { return closed_; });
        }
    }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;

    [[nodiscard]] DWORD initializationError() const noexcept { return initializationError_; }

    template <typename Operation>
    [[nodiscard]] DWORD invoke(Operation operation)
    {
        std::lock_guard lock{apiMutex_};
        if (!value_) return ERROR_WINHTTP_OPERATION_CANCELLED;
        return operation(value_) ? ERROR_SUCCESS : GetLastError();
    }

    void close() noexcept
    {
        std::lock_guard lock{apiMutex_};
        const auto value = std::exchange(value_, nullptr);
        if (value) static_cast<void>(WinHttpCloseHandle(value));
    }

    [[nodiscard]] DWORD send(std::wstring headers)
    {
        headers_ = std::move(headers);
        return submit(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&](const HINTERNET handle) {
            return WinHttpSendRequest(handle, headers_.c_str(), static_cast<DWORD>(-1L),
                body_.empty() ? WINHTTP_NO_REQUEST_DATA : body_.data(),
                static_cast<DWORD>(body_.size()), static_cast<DWORD>(body_.size()),
                reinterpret_cast<DWORD_PTR>(this));
        }).error;
    }

    [[nodiscard]] DWORD receive()
    {
        return submit(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
            [](const HINTERNET handle) { return WinHttpReceiveResponse(handle, nullptr); }).error;
    }

    struct ReadResult final { DWORD error{}; DWORD bytes{}; };
    [[nodiscard]] ReadResult read()
    {
        return submit(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&](const HINTERNET handle) {
            return WinHttpReadData(handle, readBuffer_.data(), static_cast<DWORD>(readBuffer_.size()), nullptr);
        });
    }
    [[nodiscard]] const char* readData() const noexcept { return readBuffer_.data(); }

private:
    template <typename Operation>
    [[nodiscard]] ReadResult submit(const DWORD completion, Operation operation)
    {
        {
            std::lock_guard apiLock{apiMutex_};
            if (!value_) return {ERROR_WINHTTP_OPERATION_CANCELLED, 0U};
            {
                std::lock_guard stateLock{stateMutex_};
                expectedCompletion_ = completion;
                completed_ = false;
                result_ = {};
            }
            // A callback may run inline: it uses only stateMutex_, never apiMutex_.
            if (!operation(value_)) return {GetLastError(), 0U};
        }
        std::unique_lock lock{stateMutex_};
        changed_.wait(lock, [&] { return completed_ || closed_; });
        return completed_ ? result_ : ReadResult{ERROR_WINHTTP_OPERATION_CANCELLED, 0U};
    }

    static void CALLBACK callback(HINTERNET, const DWORD_PTR context, const DWORD status,
        void* information, const DWORD length) noexcept
    {
        if (!context) return;
        auto& request = *reinterpret_cast<AsyncRequest*>(context);
        std::lock_guard lock{request.stateMutex_};
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            request.closed_ = true;
        } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            request.result_.error = information && length >= sizeof(WINHTTP_ASYNC_RESULT)
                ? static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
            request.completed_ = true;
        } else if (status == request.expectedCompletion_) {
            request.result_.bytes = status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? length : 0U;
            request.completed_ = true;
        }
        request.changed_.notify_all();
    }

    HINTERNET value_{};
    DWORD initializationError_{};
    bool callbackInstalled_{};
    std::mutex apiMutex_;
    std::mutex stateMutex_;
    std::condition_variable changed_;
    DWORD expectedCompletion_{};
    ReadResult result_;
    bool completed_{};
    bool closed_{};
    std::wstring headers_;
    std::string body_;
    std::array<char, 8192U> readBuffer_{};
};


std::string responseHeader(const std::shared_ptr<AsyncRequest>& request, DWORD query,
    const Domain::OperationContext& context, Domain::MonotonicTimePoint deadline) {
    std::wstring value(8192U, L'\0');
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const auto error = request->invoke([&](HINTERNET handle) {
        return WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX);
    });
    check(context, deadline);
    if (error == ERROR_WINHTTP_HEADER_NOT_FOUND) return {};
    if (error == ERROR_INSUFFICIENT_BUFFER) fail(Domain::ErrorCodes::PayloadTooLarge, "Response header exceeded 16 KiB.");
    if (error != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Reading a response header failed (WinHTTP " + std::to_string(error) + ").", true);
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && !value.back()) value.pop_back();
    return take(Utf::strictUtf16ToUtf8(value));
}
struct Response final {
    DWORD status{};
    Url finalUrl;
    std::string contentType, body;
    std::optional<std::uint64_t> contentLength;
    std::size_t redirects{};
    bool truncated{};
};
Response fetch(Url url, const std::string& method, std::wstring headers, const std::string& body,
    std::size_t maximum, const Domain::OperationContext& context, Domain::MonotonicTimePoint deadline) {
    const auto userAgent = take(Utf::strictUtf8ToUtf16("Forge-Conductor/" + std::string{Domain::ProductVersion}));
    InternetHandle session{WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Creating the HTTP session failed.", true);
    for (std::size_t hops = 0U;; ++hops) {
        check(context, deadline);
        InternetHandle connection{WinHttpConnect(session.get(), url.host.c_str(), url.port, 0U)};
        if (!connection.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Creating the HTTP connection failed.", true);
        const auto verb = take(Utf::strictUtf8ToUtf16(method));
        InternetHandle raw{WinHttpOpenRequest(connection.get(), verb.c_str(), url.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0U)};
        if (!raw.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Creating the HTTP request failed.", true);
        auto request = std::make_shared<AsyncRequest>(raw.get(), body);
        static_cast<void>(raw.release());
        if (request->initializationError()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Initializing asynchronous HTTP failed.", true);
        std::stop_callback cancel{context.cancellation, [request] { request->close(); }};
        std::jthread guard{[request, deadline](std::stop_token stop) noexcept {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::unique_lock lock{mutex};
            static_cast<void>(changed.wait_until(lock, stop, deadline, [] { return false; }));
            if (!stop.stop_requested()) request->close();
        }};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const auto timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 60000));
        const auto validateOperation = [&](DWORD error) {
            check(context, deadline);
            if (error == ERROR_WINHTTP_TIMEOUT) fail(Domain::ErrorCodes::DeadlineExceeded, "HTTP transport timed out.", true);
            if (error != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "HTTP transport failed (WinHTTP " + std::to_string(error) + ").", true);
        };
        validateOperation(request->invoke([&](HINTERNET handle) { return WinHttpSetTimeouts(handle, timeout, timeout, timeout, timeout); }));
        DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        validateOperation(request->invoke([&](HINTERNET handle) { return WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)); }));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        validateOperation(request->invoke([&](HINTERNET handle) { return WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)); }));
        validateOperation(request->send(headers));
        validateOperation(request->receive());
        Response result;
        result.finalUrl = url;
        result.redirects = hops;
        DWORD bytes = sizeof(result.status);
        validateOperation(request->invoke([&](HINTERNET handle) {
            return WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &bytes, WINHTTP_NO_HEADER_INDEX);
        }));
        result.contentType = responseHeader(request, WINHTTP_QUERY_CONTENT_TYPE, context, deadline);
        const auto contentLength = responseHeader(request, WINHTTP_QUERY_CONTENT_LENGTH, context, deadline);
        std::uint64_t length{};
        const auto parsedLength = std::from_chars(contentLength.data(), contentLength.data() + contentLength.size(), length);
        if (!contentLength.empty() && parsedLength.ec == std::errc{} && parsedLength.ptr == contentLength.data() + contentLength.size())
            result.contentLength = length;
        const bool redirectStatus = result.status == 301U || result.status == 302U || result.status == 303U || result.status == 307U || result.status == 308U;
        if (redirectStatus && (method == "GET" || method == "HEAD")) {
            const auto location = responseHeader(request, WINHTTP_QUERY_LOCATION, context, deadline);
            if (!location.empty()) {
                if (hops >= 5U) fail(Domain::ErrorCodes::LimitExceeded, "HTTP redirect limit of five was exceeded.");
                auto next = redirect(url, location);
                // Explicit credentials or custom secret headers must not cross origins.
                if (!sameOrigin(url, next)) headers.clear();
                url = std::move(next);
                continue;
            }
        }
        if (method != "HEAD") {
            while (true) {
                const auto read = request->read();
                validateOperation(read.error);
                if (!read.bytes) break;
                const auto available = maximum - result.body.size();
                const auto retained = std::min<std::size_t>(read.bytes, available);
                result.body.append(request->readData(), retained);
                if (read.bytes > available) { result.truncated = true; break; }
                // Read once beyond an exact bound to distinguish EOF from truncation.
            }
        }
        check(context, deadline);
        return result;
    }
}
std::string base64(std::string_view input) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < input.size(); i += 3U) {
        const auto a = static_cast<unsigned char>(input[i]);
        const auto b = i + 1U < input.size() ? static_cast<unsigned char>(input[i + 1U]) : 0U;
        const auto c = i + 2U < input.size() ? static_cast<unsigned char>(input[i + 2U]) : 0U;
        result += alphabet[a >> 2U]; result += alphabet[((a & 3U) << 4U) | (b >> 4U)];
        result += i + 1U < input.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=';
        result += i + 2U < input.size() ? alphabet[c & 63U] : '=';
    }
    return result;
}
std::string percent(std::string_view input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4U]; result += hex[c & 15U]; }
    }
    return result;
}
std::string unpercent(std::string_view input) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2U < input.size() && digit(input[i + 1U]) >= 0 && digit(input[i + 2U]) >= 0) {
            result += static_cast<char>((digit(input[i + 1U]) << 4) | digit(input[i + 2U])); i += 2U;
        } else result += input[i];
    }
    return result;
}
void appendCodepoint(std::string& result, std::uint32_t cp) {
    if (!cp || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) return;
    if (cp <= 0x7FU) result += static_cast<char>(cp);
    else if (cp <= 0x7FFU) { result += static_cast<char>(0xC0U | (cp >> 6U)); result += static_cast<char>(0x80U | (cp & 63U)); }
    else if (cp <= 0xFFFFU) { result += static_cast<char>(0xE0U | (cp >> 12U)); result += static_cast<char>(0x80U | ((cp >> 6U) & 63U)); result += static_cast<char>(0x80U | (cp & 63U)); }
    else { result += static_cast<char>(0xF0U | (cp >> 18U)); result += static_cast<char>(0x80U | ((cp >> 12U) & 63U)); result += static_cast<char>(0x80U | ((cp >> 6U) & 63U)); result += static_cast<char>(0x80U | (cp & 63U)); }
}
std::string entities(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '&') {
            const auto end = input.find(';', i + 1U);
            if (end != std::string_view::npos && end - i <= 16U) {
                const auto key = input.substr(i + 1U, end - i - 1U);
                if (key == "amp") result += '&'; else if (key == "lt") result += '<'; else if (key == "gt") result += '>';
                else if (key == "quot") result += '"'; else if (key == "apos" || key == "#39") result += '\''; else if (key == "nbsp") result += ' ';
                else if (!key.empty() && key.front() == '#') {
                    const bool hex = key.size() > 1U && (key[1] == 'x' || key[1] == 'X');
                    auto digits = key.substr(hex ? 2U : 1U);
                    std::uint32_t cp{};
                    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
                    if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()) appendCodepoint(result, cp);
                    else { result.append(input.substr(i, end - i + 1U)); }
                } else { result.append(input.substr(i, end - i + 1U)); }
                i = end; continue;
            }
        }
        result += input[i];
    }
    return result;
}
// This is bounded plain-text extraction, not a DOM renderer or executable page.
std::string plain(std::string_view html, std::size_t maximum = 65536U) {
    std::string stripped;
    bool tag = false;
    for (const char c : html) {
        if (c == '<') { tag = true; stripped += ' '; }
        else if (c == '>') tag = false;
        else if (!tag) stripped += c;
    }
    auto decoded = entities(stripped);
    std::string result;
    bool space = false;
    for (const unsigned char c : decoded) {
        if (c <= 32U) { space = !result.empty(); continue; }
        if (space) { result += ' '; space = false; }
        result += static_cast<char>(c);
        if (result.size() >= maximum) break;
    }
    while (!result.empty() && !Domain::isValidUtf8(result)) result.pop_back();
    return result;
}
std::string attribute(std::string_view tag, std::string_view wanted) {
    std::size_t i = 1U;
    while (i < tag.size() && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n') ++i;
    while (i < tag.size()) {
        while (i < tag.size() && (static_cast<unsigned char>(tag[i]) <= 32U || tag[i] == '/')) ++i;
        const auto begin = i;
        while (i < tag.size() && tag[i] != '=' && static_cast<unsigned char>(tag[i]) > 32U && tag[i] != '>') ++i;
        const auto name = lower(std::string{tag.substr(begin, i - begin)});
        while (i < tag.size() && static_cast<unsigned char>(tag[i]) <= 32U) ++i;
        if (i >= tag.size() || tag[i] != '=') { if (i < tag.size()) ++i; continue; }
        ++i;
        while (i < tag.size() && static_cast<unsigned char>(tag[i]) <= 32U) ++i;
        if (i >= tag.size()) break;
        const char quote = tag[i] == '\'' || tag[i] == '"' ? tag[i++] : '\0';
        const auto value = i;
        while (i < tag.size() && (quote ? tag[i] != quote : static_cast<unsigned char>(tag[i]) > 32U && tag[i] != '>')) ++i;
        if (name == wanted) return entities(tag.substr(value, i - value));
        if (quote && i < tag.size()) ++i;
    }
    return {};
}
Json searchResults(const Response& response, std::size_t limit, bool& bounded) {
    Json results = Json::array();
    if (!Domain::isValidUtf8(response.body)) return results;
    const auto folded = lower(response.body);
    std::set<std::string> seen;
    std::size_t cursor{};
    while (results.size() < limit) {
        const auto start = folded.find("<a ", cursor);
        if (start == std::string::npos) break;
        const auto tagEnd = folded.find('>', start);
        const auto end = folded.find("</a>", tagEnd);
        if (tagEnd == std::string::npos || end == std::string::npos) break;
        cursor = end + 4U;
        const std::string_view tag{response.body.data() + start, tagEnd - start + 1U};
        const auto classes = attribute(tag, "class");
        if (classes != "result-link" && classes != "result__a") continue;
        auto href = attribute(tag, "href");
        const auto marker = href.find("uddg=");
        if (marker != std::string::npos) {
            const auto value = marker + 5U;
            const auto amp = href.find('&', value);
            href = unpercent(std::string_view{href}.substr(value, amp == std::string::npos ? href.size() - value : amp - value));
        }
        try {
            auto link = href.starts_with("http://") || href.starts_with("https://") ? parseUrl(href) : redirect(response.finalUrl, href);
            if (!seen.insert(link.encoded).second) continue;
            auto title = plain(std::string_view{response.body}.substr(tagEnd + 1U, end - tagEnd - 1U), 2048U);
            if (title.empty()) continue;
            std::string snippet;
            const auto nextAnchor = folded.find("<a ", cursor);
            const auto snippetClass = folded.find("result-snippet", cursor);
            if (snippetClass != std::string::npos && (nextAnchor == std::string::npos || snippetClass < nextAnchor)) {
                const auto snippetStart = folded.find('>', snippetClass);
                const auto snippetEnd = folded.find("</td>", snippetStart);
                if (snippetStart != std::string::npos && snippetEnd != std::string::npos)
                    snippet = plain(std::string_view{response.body}.substr(snippetStart + 1U, snippetEnd - snippetStart - 1U), 4096U);
            }
            results.push_back({{"title", title}, {"url", link.encoded}, {"snippet", snippet}, {"source_url", response.finalUrl.encoded}});
            if (results.dump().size() > 28U * 1024U) {
                results.erase(results.size() - 1U);
                bounded = true;
                break;
            }
        } catch (const Failure&) { /* Reject non-HTTP result links without inventing replacements. */ }
    }
    return results;
}
Json envelope(const Response& response) {
    Json result{{"ok", response.status >= 200U && response.status < 300U}, {"status", response.status},
        {"final_url", response.finalUrl.encoded}, {"content_type", response.contentType},
        {"bytes_returned", response.body.size()}, {"truncated", response.truncated}, {"redirects", response.redirects}};
    result["content_length"] = response.contentLength ? Json(*response.contentLength) : Json(nullptr);
    const bool controls = std::any_of(response.body.begin(), response.body.end(), [](unsigned char c) { return c < 32U && c != '\t' && c != '\r' && c != '\n'; });
    if (Domain::isValidUtf8(response.body) && !controls) {
        result["encoding"] = "utf-8"; result["body_utf8"] = response.body;
    } else { result["encoding"] = "base64"; result["body_base64"] = base64(response.body); }
    return result;
}
} // namespace

WindowsWebAccessService::WindowsWebAccessService(std::string searchEndpoint)
{
    try { searchEndpoint_ = parseUrl(std::move(searchEndpoint)).encoded; }
    catch (const Failure& error) { throw std::invalid_argument{error.error.message}; }
}

Domain::Result<std::string> WindowsWebAccessService::execute(std::string_view toolName,
    std::string_view arguments, const Domain::OperationContext& context) noexcept {
    try {
        check(context, context.deadline);
        if (arguments.empty() || arguments.size() > 1024U * 1024U || !Domain::isValidUtf8(arguments))
            fail(Domain::ErrorCodes::InvalidRequest, "Web arguments must be bounded UTF-8 JSON.");
        auto args = Json::parse(arguments);
        if (!args.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "Web arguments must be a JSON object.");
        const auto seconds = number(args, "timeout_sec", 30U, 60U);
        const auto deadline = std::min(context.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{seconds});
        const bool search = toolName == "web_search";
        if (!search && toolName != "web_fetch" && toolName != "http_request")
            fail(Domain::ErrorCodes::InvalidRequest, "Unknown web tool.");
        std::string query;
        Url url;
        if (search) {
            query = text(args, "query", 2048U);
            if (std::any_of(query.begin(), query.end(), [](unsigned char c) { return c < 32U || c == 127U; }) ||
                std::all_of(query.begin(), query.end(), [](unsigned char c) { return c == ' '; }))
                fail(Domain::ErrorCodes::InvalidRequest, "query must contain visible text without controls.");
            url = parseUrl(searchEndpoint_ + (searchEndpoint_.find('?') == std::string::npos ? "?q=" : "&q=") + percent(query));
        } else url = parseUrl(text(args, "url", 8192U));
        std::string method = "GET", body;
        std::wstring headers;
        if (toolName == "http_request") {
            method = args.contains("method") ? text(args, "method", 7U) : "GET";
            if (method != "GET" && method != "HEAD" && method != "POST" && method != "PUT" &&
                method != "PATCH" && method != "DELETE" && method != "OPTIONS")
                fail(Domain::ErrorCodes::InvalidRequest, "method must be GET, HEAD, POST, PUT, PATCH, DELETE, or OPTIONS.");
            body = text(args, "body", MaximumRequestBodyBytes, false);
            if (!body.empty() && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE")
                fail(Domain::ErrorCodes::InvalidRequest, "Only POST, PUT, PATCH, or DELETE accepts a request body.");
            headers = requestHeaders(args);
        }
        const auto maximum = search ? MaximumBodyBytes : number(args, "max_bytes", DefaultBodyBytes, MaximumBodyBytes);
        const auto limit = search ? number(args, "limit", 5U, 10U) : 0U;
        auto response = fetch(std::move(url), method, std::move(headers), body, maximum, context, deadline);
        Json result;
        if (search) {
            bool bounded = false;
            const auto results = searchResults(response, limit, bounded);
            const bool available = response.status == 200U && !results.empty();
            result = {{"ok", available}, {"status", response.status}, {"query", query},
                {"provider", "DuckDuckGo Lite HTML"}, {"source_url", response.finalUrl.encoded},
                {"search_available", available}, {"results", results}, {"truncated", response.truncated || bounded}};
            if (!available) result["diagnostic"] = response.status != 200U
                ? "Search endpoint returned an unsuccessful HTTP status or challenge."
                : "Search endpoint returned no recognized result links; it may be empty, challenged, or have changed format.";
        } else result = envelope(response);
        check(context, deadline);
        return Domain::Result<std::string>::success(result.dump());
    } catch (const Failure& error) { return Domain::Result<std::string>::failure(error.error); }
      catch (const nlohmann::json::exception&) {
        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Web arguments contain invalid JSON or field types."));
    } catch (const std::exception&) {
        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Web request failed unexpectedly."));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Web request failed unexpectedly."));
    }
}
} // namespace ForgeConductor::NativeTools::Windows
