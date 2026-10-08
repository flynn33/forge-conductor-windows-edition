#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderHttpTransport.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace ForgeConductor::NativeTools::Windows {
namespace {
namespace Utf = Infrastructure::Windows::Detail;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message, bool retryable = false) {
    throw Failure{Domain::makeError(code, std::move(message), retryable)};
}
template<class T> T take(Domain::Result<T> value) { if (!value) throw Failure{value.error()}; return std::move(value).value(); }
void check(const Domain::OperationContext& context, Domain::MonotonicTimePoint deadline) {
    auto valid = Utf::validateOperationContext(context, std::chrono::steady_clock::now(), "image provider HTTP");
    if (!valid) throw Failure{valid.error()};
    if (std::chrono::steady_clock::now() >= deadline) fail(Domain::ErrorCodes::DeadlineExceeded, "Image provider HTTP deadline expired.");
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

} // namespace

Domain::Result<Contracts::ImageProviderHttpResponse> WindowsImageProviderHttpTransport::request(
    const Domain::ImageProviderConfig& provider, std::string_view method, std::string_view route,
    std::string_view contentType, std::span<const std::byte> body, std::size_t maximum,
    const Domain::OperationContext& context) noexcept {
    try {
        check(context, context.deadline);
        auto valid = Domain::validateImageProviderConfig(provider); if (!valid) throw Failure{valid.error()};
        if (provider.endpoint.empty() || (method != "GET" && method != "POST") ||
            route.empty() || route.front() != '/' || route.size() > 4096U ||
            std::any_of(route.begin(), route.end(), [](unsigned char c) { return c < 33U || c > 126U || c == '#' || c == '\\'; }) ||
            body.size() > 16U * 1024U * 1024U + 4096U || maximum == 0U || maximum > 16U * 1024U * 1024U ||
            (method == "GET" && !body.empty()) || contentType.size() > 256U ||
            std::any_of(contentType.begin(), contentType.end(), [](unsigned char c) { return c < 32U || c > 126U; }))
            fail(Domain::ErrorCodes::InvalidRequest, "Invalid bounded image-provider HTTP request.");
        auto url = take(Utf::strictUtf8ToUtf16(provider.endpoint + std::string{route}));
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0U, &parts))
            fail(Domain::ErrorCodes::InvalidRequest, "Invalid image-provider endpoint.");
        std::wstring host{parts.lpszHostName, parts.dwHostNameLength};
        std::wstring path{parts.lpszUrlPath, parts.dwUrlPathLength};
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        auto agent = take(Utf::strictUtf8ToUtf16("Forge-Conductor/" + std::string{Domain::ProductVersion}));
        InternetHandle session{::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
        if (!session.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Creating image-provider HTTP session failed.");
        InternetHandle connection{::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0U)};
        if (!connection.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Connecting image provider failed.");
        auto verb = take(Utf::strictUtf8ToUtf16(method));
        InternetHandle raw{::WinHttpOpenRequest(connection.get(), verb.c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0U)};
        if (!raw.get()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Creating image-provider request failed.");
        std::string payload;
        if (!body.empty()) payload.assign(reinterpret_cast<const char*>(body.data()), body.size());
        auto request = std::make_shared<AsyncRequest>(raw.get(), std::move(payload));
        static_cast<void>(raw.release());
        if (request->initializationError()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Initializing image-provider HTTP failed.");
        std::stop_callback cancel{context.cancellation, [request] { request->close(); }};
        std::jthread guard{[request, deadline = context.deadline](std::stop_token stop) {
            std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock{mutex};
            static_cast<void>(changed.wait_until(lock, stop, deadline, [] { return false; }));
            if (!stop.stop_requested()) request->close();
        }};
        const auto timeout = static_cast<int>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - std::chrono::steady_clock::now()).count(), 1, 60000));
        const auto operation = [&](DWORD error) {
            check(context, context.deadline);
            if (error == ERROR_WINHTTP_TIMEOUT) fail(Domain::ErrorCodes::DeadlineExceeded, "Image-provider HTTP timed out.");
            if (error != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Image-provider HTTP failed (WinHTTP " + std::to_string(error) + ").");
        };
        operation(request->invoke([&](HINTERNET h) { return ::WinHttpSetTimeouts(h, timeout, timeout, timeout, timeout); }));
        DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        operation(request->invoke([&](HINTERNET h) { return ::WinHttpSetOption(h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)); }));
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        operation(request->invoke([&](HINTERNET h) { return ::WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)); }));
        std::wstring headers;
        if (!contentType.empty()) headers = take(Utf::strictUtf8ToUtf16("Content-Type: " + std::string{contentType} + "\r\n"));
        operation(request->send(std::move(headers))); operation(request->receive());
        Contracts::ImageProviderHttpResponse result;
        DWORD count = sizeof(result.status);
        operation(request->invoke([&](HINTERNET h) { return ::WinHttpQueryHeaders(h,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &result.status, &count, WINHTTP_NO_HEADER_INDEX); }));
        result.contentType = responseHeader(request, WINHTTP_QUERY_CONTENT_TYPE, context, context.deadline);
        const auto contentLength = responseHeader(request, WINHTTP_QUERY_CONTENT_LENGTH, context, context.deadline);
        std::uint64_t declared{};
        if (!contentLength.empty()) {
            const auto parsed = std::from_chars(contentLength.data(), contentLength.data() + contentLength.size(), declared);
            if (parsed.ec != std::errc{} || parsed.ptr != contentLength.data() + contentLength.size())
                fail(Domain::ErrorCodes::MalformedMessage, "Image-provider Content-Length is invalid.");
            if (declared > maximum) fail(Domain::ErrorCodes::PayloadTooLarge, "Image-provider response exceeds its byte bound.");
        }
        for (;;) {
            const auto read = request->read(); operation(read.error); if (!read.bytes) break;
            if (read.bytes > maximum - result.body.size())
                fail(Domain::ErrorCodes::PayloadTooLarge, "Image-provider response exceeds its byte bound.");
            const auto bytes = reinterpret_cast<const std::byte*>(request->readData());
            result.body.insert(result.body.end(), bytes, bytes + read.bytes);
        }
        if (!contentLength.empty() && result.body.size() != declared)
            fail(Domain::ErrorCodes::MalformedMessage, "Image-provider response is incomplete.");
        check(context, context.deadline);
        return Domain::Result<Contracts::ImageProviderHttpResponse>::success(std::move(result));
    } catch (const Failure& error) { return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(error.error); }
    catch (...) { return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, "Image-provider HTTP failed.")); }
}
} // namespace ForgeConductor::NativeTools::Windows
