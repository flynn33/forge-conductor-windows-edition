#include "ForgeConductor/Mcp/WindowsStdioMcpTransport.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Mcp {
namespace {

using ReceiveValue = std::optional<Domain::McpFrame>;

constexpr std::size_t ReadChunkBytes = 4096U;
constexpr DWORD CancellationPollMilliseconds = 10U;
constexpr DWORD OperationStateAuditMilliseconds = 250U;
constexpr auto CancellationJoinGrace = std::chrono::seconds{1};

class OwnedHandle final {
public:
    OwnedHandle() noexcept = default;
    explicit OwnedHandle(const HANDLE handle) noexcept : handle_{handle} {}
    ~OwnedHandle() noexcept { reset(); }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    OwnedHandle(OwnedHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    OwnedHandle& operator=(OwnedHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(const HANDLE replacement = nullptr) noexcept
    {
        const HANDLE previous = std::exchange(handle_, replacement);
        if (previous != nullptr && previous != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(previous));
        }
    }

private:
    HANDLE handle_{};
};

[[nodiscard]] Domain::Error transportError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error internalTransportError()
{
    return transportError(
        Domain::ErrorCodes::InternalFailure,
        "The MCP stdio transport encountered a native I/O failure.",
        true);
}

[[nodiscard]] Domain::Error malformedFrameError(std::string message)
{
    return transportError(
        Domain::ErrorCodes::MalformedMessage,
        std::move(message));
}

[[nodiscard]] Domain::Error payloadTooLargeError()
{
    return transportError(
        Domain::ErrorCodes::PayloadTooLarge,
        "The MCP stdio frame exceeds 1,048,576 bytes.");
}

[[nodiscard]] Domain::Error brokenPipeError()
{
    return transportError(
        Domain::ErrorCodes::TransportClosed,
        "The MCP stdio peer closed the transport.");
}

[[nodiscard]] bool isBrokenPipeError(const DWORD error) noexcept
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
           error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_INVALID_HANDLE;
}

[[nodiscard]] bool isSupportedStdioHandle(const HANDLE handle) noexcept
{
    const DWORD type = ::GetFileType(handle);
    return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

void detachRetainedWorker(std::thread& worker) noexcept
{
    if (!worker.joinable()) {
        return;
    }
    try {
        worker.detach();
    } catch (...) {
        // Losing both the join and retained-state ownership invariants is not
        // recoverable without risking a use-after-free from a joinable thread.
        std::terminate();
    }
}

[[nodiscard]] bool startsWithContentLength(const std::string_view line) noexcept
{
    constexpr std::string_view Header{"content-length"};
    std::size_t offset{};
    while (offset < line.size() &&
           (line[offset] == ' ' || line[offset] == '\t')) {
        ++offset;
    }
    if (line.size() - offset <= Header.size()) {
        return false;
    }
    for (std::size_t index{}; index < Header.size(); ++index) {
        const auto value = static_cast<unsigned char>(line[offset + index]);
        const auto lowered = value >= 'A' && value <= 'Z'
                                 ? static_cast<unsigned char>(value + ('a' - 'A'))
                                 : value;
        if (lowered != static_cast<unsigned char>(Header[index])) {
            return false;
        }
    }
    return line[offset + Header.size()] == ':';
}

[[nodiscard]] DWORD waitMillisecondsUntil(
    const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }

    const auto remaining = deadline - now;
    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) {
        ++milliseconds;
    }
    constexpr auto MaximumWait =
        static_cast<long long>(INFINITE) - 1LL;
    return milliseconds.count() >= MaximumWait
               ? INFINITE - 1U
               : static_cast<DWORD>(std::max<long long>(1LL, milliseconds.count()));
}

enum class AbortReason {
    Shutdown,
    Cancelled,
    Deadline,
    WaitFailure,
};

[[nodiscard]] Domain::Error abortError(const AbortReason reason)
{
    switch (reason) {
    case AbortReason::Shutdown:
        return transportError(
            Domain::ErrorCodes::TransportClosed,
            "The MCP stdio transport is closed.");
    case AbortReason::Cancelled:
        return transportError(
            Domain::ErrorCodes::Cancelled,
            "The MCP stdio operation was cancelled.");
    case AbortReason::Deadline:
        return transportError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The MCP stdio operation deadline expired.");
    case AbortReason::WaitFailure:
        return internalTransportError();
    }
    return internalTransportError();
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(const Domain::Error& error)
{
    return Domain::Result<T>::failure(error);
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(Domain::Error&& error)
{
    return Domain::Result<T>::failure(std::move(error));
}

} // namespace

class WindowsStdioMcpTransport::Impl final
    : public std::enable_shared_from_this<WindowsStdioMcpTransport::Impl> {
public:
    Impl(
        OwnedHandle inputHandle,
        OwnedHandle outputHandle,
        OwnedHandle shutdownEvent) noexcept
        : inputHandle_{std::move(inputHandle)},
          outputHandle_{std::move(outputHandle)},
          shutdownEvent_{std::move(shutdownEvent)}
    {
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] Domain::Result<ReceiveValue> receive(
        const Domain::OperationContext& context) noexcept
    {
        if (!beginOperation()) {
            return failure<ReceiveValue>(abortError(AbortReason::Shutdown));
        }
        const OperationLease operationLease{*this};
        try {
            std::unique_lock lock{receiveMutex_, std::defer_lock};
            auto locked = lockOperation(lock, context);
            if (!locked) {
                return failure<ReceiveValue>(std::move(locked).error());
            }
            return runCancelable<ReceiveValue>(
                context,
                [this](std::atomic_bool& abortRequested) {
                    return readFrame(abortRequested);
                });
        } catch (...) {
            return failure<ReceiveValue>(internalTransportError());
        }
    }

    [[nodiscard]] Domain::Result<void> send(
        const Domain::McpFrame& frame,
        const Domain::OperationContext& context) noexcept
    {
        if (!beginOperation()) {
            return Domain::Result<void>::failure(
                abortError(AbortReason::Shutdown));
        }
        const OperationLease operationLease{*this};
        try {
            auto ready = operationReady(context);
            if (!ready) {
                return ready;
            }
            auto valid = validateOutboundFrame(frame);
            if (!valid) {
                return valid;
            }

            std::unique_lock lock{sendMutex_, std::defer_lock};
            auto locked = lockOperation(lock, context);
            if (!locked) {
                return locked;
            }

            std::string wire{frame.utf8Json};
            wire.push_back('\n');
            auto sent = runCancelable<void>(
                context,
                [this, wire = std::move(wire)](
                    std::atomic_bool& abortRequested) {
                    return writeFrame(wire, abortRequested);
                });
            if (!sent) {
                // Synchronous WriteFile cancellation cannot prove that zero
                // bytes reached the peer. Close admission so no later frame
                // can be appended to a potentially truncated JSON object.
                shutdown();
            }
            return sent;
        } catch (...) {
            return Domain::Result<void>::failure(internalTransportError());
        }
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard lock{activeMutex_};
            shutdownRequested_.store(true, std::memory_order_release);
        }
        if (shutdownEvent_.valid()) {
            static_cast<void>(::SetEvent(shutdownEvent_.get()));
        }
    }

    void waitForOperations() noexcept
    {
        try {
            std::unique_lock lock{activeMutex_};
            activeChanged_.wait(lock, [this] { return activeOperations_ == 0U; });
        } catch (...) {
        }
    }

private:
    class OperationLease final {
    public:
        explicit OperationLease(Impl& owner) noexcept : owner_{owner} {}
        ~OperationLease() noexcept { owner_.finishOperation(); }

        OperationLease(const OperationLease&) = delete;
        OperationLease& operator=(const OperationLease&) = delete;

    private:
        Impl& owner_;
    };

    [[nodiscard]] bool beginOperation() noexcept
    {
        try {
            std::lock_guard lock{activeMutex_};
            if (shutdownRequested_.load(std::memory_order_acquire)) {
                return false;
            }
            ++activeOperations_;
            return true;
        } catch (...) {
            return false;
        }
    }

    void finishOperation() noexcept
    {
        try {
            std::lock_guard lock{activeMutex_};
            if (activeOperations_ > 0U) {
                --activeOperations_;
            }
            if (activeOperations_ == 0U) {
                activeChanged_.notify_all();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::Result<void> operationReady(
        const Domain::OperationContext& context) const
    {
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return Domain::Result<void>::failure(abortError(AbortReason::Shutdown));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(abortError(AbortReason::Cancelled));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(abortError(AbortReason::Deadline));
        }
        return Domain::Result<void>::success();
    }

    template <typename Mutex>
    [[nodiscard]] Domain::Result<void> lockOperation(
        std::unique_lock<Mutex>& lock,
        const Domain::OperationContext& context)
    {
        for (;;) {
            auto ready = operationReady(context);
            if (!ready) {
                return ready;
            }
            const DWORD remaining = waitMillisecondsUntil(context.deadline);
            if (remaining == 0U) {
                return Domain::Result<void>::failure(
                    abortError(AbortReason::Deadline));
            }
            const auto slice = std::chrono::milliseconds{
                std::min(remaining, CancellationPollMilliseconds)};
            if (lock.try_lock_for(slice)) {
                return operationReady(context);
            }
        }
    }

    template <typename T, typename Operation>
    [[nodiscard]] Domain::Result<T> runCancelable(
        const Domain::OperationContext& context,
        Operation&& operation)
    {
        auto ready = operationReady(context);
        if (!ready) {
            return failure<T>(std::move(ready).error());
        }

        struct WorkerState final {
            OwnedHandle completedEvent;
            std::atomic_bool abortRequested{};
            std::optional<Domain::Result<T>> result;
        };

        auto state = std::make_shared<WorkerState>();
        state->completedEvent.reset(
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OwnedHandle cancellationEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!state->completedEvent.valid() || !cancellationEvent.valid()) {
            return failure<T>(internalTransportError());
        }

        std::stop_callback cancellationCallback{
            context.cancellation,
            [event = cancellationEvent.get()]() noexcept {
                static_cast<void>(::SetEvent(event));
            }};

        std::thread worker;
        try {
            auto owner = shared_from_this();
            worker = std::thread{
                [state,
                 owner = std::move(owner),
                 operation = std::forward<Operation>(operation)]() mutable noexcept {
                    try {
                        state->result.emplace(
                            operation(state->abortRequested));
                    } catch (...) {
                        try {
                            state->result.emplace(
                                Domain::Result<T>::failure(internalTransportError()));
                        } catch (...) {
                        }
                    }
                    static_cast<void>(
                        ::SetEvent(state->completedEvent.get()));
                }};
        } catch (...) {
            return failure<T>(internalTransportError());
        }

        std::optional<AbortReason> abortReason;
        const std::array<HANDLE, 3U> waitHandles{
            state->completedEvent.get(),
            shutdownEvent_.get(),
            cancellationEvent.get()};

        while (!abortReason.has_value()) {
            const DWORD remaining = waitMillisecondsUntil(context.deadline);
            if (remaining == 0U) {
                abortReason = AbortReason::Deadline;
                break;
            }
            const DWORD waitTimeout =
                std::min(remaining, OperationStateAuditMilliseconds);
            const DWORD waited = ::WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                FALSE,
                waitTimeout);
            if (waited == WAIT_OBJECT_0) {
                break;
            }
            if (waited == WAIT_OBJECT_0 + 1U) {
                abortReason = AbortReason::Shutdown;
                break;
            }
            if (waited == WAIT_OBJECT_0 + 2U) {
                abortReason = AbortReason::Cancelled;
                break;
            }
            if (waited == WAIT_TIMEOUT) {
                if (shutdownRequested_.load(std::memory_order_acquire)) {
                    abortReason = AbortReason::Shutdown;
                } else if (context.isCancellationRequested()) {
                    abortReason = AbortReason::Cancelled;
                } else if (context.isExpired(std::chrono::steady_clock::now())) {
                    abortReason = AbortReason::Deadline;
                }
                continue;
            }
            abortReason = AbortReason::WaitFailure;
        }

        if (abortReason.has_value()) {
            state->abortRequested.store(true, std::memory_order_release);
            const auto joinDeadline =
                std::chrono::steady_clock::now() + CancellationJoinGrace;
            bool completed{};
            while (std::chrono::steady_clock::now() < joinDeadline) {
                const DWORD completion = ::WaitForSingleObject(
                    state->completedEvent.get(), 0U);
                if (completion == WAIT_OBJECT_0) {
                    completed = true;
                    break;
                }
                if (completion == WAIT_FAILED) {
                    break;
                }
                if (::CancelSynchronousIo(worker.native_handle()) == FALSE) {
                    const DWORD cancelError = ::GetLastError();
                    if (cancelError != ERROR_NOT_FOUND) {
                        break;
                    }
                }
                const DWORD waited = ::WaitForSingleObject(
                    state->completedEvent.get(),
                    CancellationPollMilliseconds);
                if (waited == WAIT_OBJECT_0) {
                    completed = true;
                    break;
                }
                if (waited == WAIT_FAILED) {
                    break;
                }
            }
            if (!completed &&
                ::WaitForSingleObject(state->completedEvent.get(), 0U) ==
                    WAIT_OBJECT_0) {
                completed = true;
            }
            if (!completed) {
                // Poison admission before retaining a self-owned worker whose
                // native I/O provider did not honor cancellation. A one-shot
                // serve process can retain at most one reader and one writer.
                shutdown();
                detachRetainedWorker(worker);
                return failure<T>(abortError(*abortReason));
            }
        }

        worker.join();
        if (abortReason.has_value()) {
            return failure<T>(abortError(*abortReason));
        }
        if (!state->result.has_value()) {
            return failure<T>(internalTransportError());
        }
        return std::move(*state->result);
    }

    [[nodiscard]] Domain::Result<ReceiveValue> readFrame(
        std::atomic_bool& abortRequested)
    {
        for (;;) {
            if (discardingOversizedFrame_) {
                const auto newline = inputBuffer_.find('\n');
                if (newline != std::string::npos) {
                    inputBuffer_.erase(0U, newline + 1U);
                    discardingOversizedFrame_ = false;
                    return failure<ReceiveValue>(payloadTooLargeError());
                }
                inputBuffer_.clear();
            } else {
                const auto newline = inputBuffer_.find('\n');
                if (newline != std::string::npos) {
                    if (newline > MaximumFrameBytes) {
                        inputBuffer_.erase(0U, newline + 1U);
                        return failure<ReceiveValue>(payloadTooLargeError());
                    }
                    std::string line = inputBuffer_.substr(0U, newline);
                    inputBuffer_.erase(0U, newline + 1U);
                    return validateInboundFrame(std::move(line));
                }
                if (inputBuffer_.size() > MaximumFrameBytes) {
                    inputBuffer_.clear();
                    discardingOversizedFrame_ = true;
                }
            }

            if (abortRequested.load(std::memory_order_acquire)) {
                return failure<ReceiveValue>(
                    abortError(AbortReason::Cancelled));
            }

            std::array<char, ReadChunkBytes> chunk{};
            DWORD bytesRead{};
            const BOOL read = ::ReadFile(
                inputHandle_.get(),
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &bytesRead,
                nullptr);
            if (read == FALSE) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_OPERATION_ABORTED &&
                    abortRequested.load(std::memory_order_acquire)) {
                    return failure<ReceiveValue>(
                        abortError(AbortReason::Cancelled));
                }
                if (isBrokenPipeError(error)) {
                    return endOfInput();
                }
                return failure<ReceiveValue>(internalTransportError());
            }
            if (bytesRead == 0U) {
                return endOfInput();
            }
            inputBuffer_.append(chunk.data(), bytesRead);
        }
    }

    [[nodiscard]] Domain::Result<ReceiveValue> endOfInput()
    {
        if (!inputBuffer_.empty() || discardingOversizedFrame_) {
            inputBuffer_.clear();
            discardingOversizedFrame_ = false;
            return failure<ReceiveValue>(malformedFrameError(
                "The MCP stdio stream ended before the frame newline."));
        }
        return Domain::Result<ReceiveValue>::success(std::nullopt);
    }

    [[nodiscard]] Domain::Result<ReceiveValue> validateInboundFrame(
        std::string line) const
    {
        if (line.empty()) {
            return failure<ReceiveValue>(malformedFrameError(
                "Empty MCP stdio lines are not valid frames."));
        }
        if (line.find('\r') != std::string::npos ||
            line.find('\0') != std::string::npos) {
            return failure<ReceiveValue>(malformedFrameError(
                "MCP stdio frames must not contain CR or NUL bytes."));
        }
        if (startsWithContentLength(line)) {
            return failure<ReceiveValue>(malformedFrameError(
                "Content-Length framing is not supported by MCP stdio."));
        }

        if (!Domain::isValidUtf8(line)) {
            return failure<ReceiveValue>(malformedFrameError(
                "MCP stdio frames must contain strict UTF-8."));
        }
        return Domain::Result<ReceiveValue>::success(
            ReceiveValue{Domain::McpFrame{std::move(line)}});
    }

    [[nodiscard]] Domain::Result<void> validateOutboundFrame(
        const Domain::McpFrame& frame) const
    {
        if (frame.utf8Json.size() > MaximumFrameBytes) {
            return Domain::Result<void>::failure(payloadTooLargeError());
        }
        if (frame.utf8Json.empty() ||
            frame.utf8Json.find('\n') != std::string::npos ||
            frame.utf8Json.find('\r') != std::string::npos ||
            frame.utf8Json.find('\0') != std::string::npos) {
            return Domain::Result<void>::failure(malformedFrameError(
                "MCP output must be one nonempty JSON line."));
        }

        McpJsonCodec codec;
        auto canonical = codec.canonicalize(frame.utf8Json);
        if (!canonical) {
            return Domain::Result<void>::failure(std::move(canonical).error());
        }
        if (canonical.value() != frame.utf8Json) {
            return Domain::Result<void>::failure(malformedFrameError(
                "MCP output must be compact canonical JSON."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> writeFrame(
        const std::string_view wire,
        std::atomic_bool& abortRequested) const
    {
        std::size_t offset{};
        while (offset < wire.size()) {
            if (abortRequested.load(std::memory_order_acquire)) {
                return Domain::Result<void>::failure(
                    abortError(AbortReason::Cancelled));
            }
            const auto remaining = wire.size() - offset;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD bytesWritten{};
            const BOOL written = ::WriteFile(
                outputHandle_.get(),
                wire.data() + offset,
                requested,
                &bytesWritten,
                nullptr);
            if (written == FALSE) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_OPERATION_ABORTED &&
                    abortRequested.load(std::memory_order_acquire)) {
                    return Domain::Result<void>::failure(
                        abortError(AbortReason::Cancelled));
                }
                return Domain::Result<void>::failure(
                    isBrokenPipeError(error)
                        ? brokenPipeError()
                        : internalTransportError());
            }
            if (bytesWritten == 0U) {
                return Domain::Result<void>::failure(brokenPipeError());
            }
            offset += bytesWritten;
        }
        return Domain::Result<void>::success();
    }

    OwnedHandle inputHandle_;
    OwnedHandle outputHandle_;
    OwnedHandle shutdownEvent_;
    std::atomic_bool shutdownRequested_{};
    std::mutex activeMutex_;
    std::condition_variable activeChanged_;
    std::size_t activeOperations_{};
    std::timed_mutex receiveMutex_;
    std::timed_mutex sendMutex_;
    std::string inputBuffer_;
    bool discardingOversizedFrame_{};
};

WindowsStdioMcpTransport::WindowsStdioMcpTransport(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsStdioMcpTransport::~WindowsStdioMcpTransport() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
        implementation->waitForOperations();
    }
}

Domain::Result<std::unique_ptr<WindowsStdioMcpTransport>>
WindowsStdioMcpTransport::create() noexcept
{
    try {
        const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE ||
            output == nullptr || output == INVALID_HANDLE_VALUE) {
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                transportError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The process does not have usable standard I/O handles."));
        }

        const HANDLE process = ::GetCurrentProcess();
        HANDLE duplicatedInput{};
        if (::DuplicateHandle(
                process,
                input,
                process,
                &duplicatedInput,
                0U,
                FALSE,
                DUPLICATE_SAME_ACCESS) == FALSE) {
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                transportError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "Standard input could not be duplicated for MCP stdio."));
        }
        OwnedHandle inputOwner{duplicatedInput};

        HANDLE duplicatedOutput{};
        if (::DuplicateHandle(
                process,
                output,
                process,
                &duplicatedOutput,
                0U,
                FALSE,
                DUPLICATE_SAME_ACCESS) == FALSE) {
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                transportError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "Standard output could not be duplicated for MCP stdio."));
        }
        OwnedHandle outputOwner{duplicatedOutput};
        return createFromOwnedHandles(
            inputOwner.release(), outputOwner.release());
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<WindowsStdioMcpTransport>>::failure(
            internalTransportError());
    }
}

Domain::Result<std::unique_ptr<WindowsStdioMcpTransport>>
WindowsStdioMcpTransport::createFromOwnedHandles(
    const NativeHandle inputHandle,
    const NativeHandle outputHandle) noexcept
{
    OwnedHandle inputOwner{static_cast<HANDLE>(inputHandle)};
    OwnedHandle outputOwner{static_cast<HANDLE>(outputHandle)};
    try {
        if (!inputOwner.valid() || !outputOwner.valid() ||
            inputHandle == outputHandle) {
            if (inputHandle == outputHandle) {
                static_cast<void>(outputOwner.release());
            }
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                transportError(
                    Domain::ErrorCodes::InvalidRequest,
                    "MCP stdio requires distinct valid input and output handles."));
        }
        if (!isSupportedStdioHandle(inputOwner.get()) ||
            !isSupportedStdioHandle(outputOwner.get())) {
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                transportError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "MCP stdio requires pipe or redirected-file handles."));
        }

        OwnedHandle shutdownEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!shutdownEvent.valid()) {
            return Domain::Result<
                std::unique_ptr<WindowsStdioMcpTransport>>::failure(
                internalTransportError());
        }

        auto implementation = std::make_shared<Impl>(
            std::move(inputOwner),
            std::move(outputOwner),
            std::move(shutdownEvent));
        auto transport = std::unique_ptr<WindowsStdioMcpTransport>{
            new WindowsStdioMcpTransport{std::move(implementation)}};
        return Domain::Result<
            std::unique_ptr<WindowsStdioMcpTransport>>::success(
            std::move(transport));
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<WindowsStdioMcpTransport>>::failure(
            internalTransportError());
    }
}

Domain::Result<ReceiveValue> WindowsStdioMcpTransport::receive(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->receive(context);
}

Domain::Result<void> WindowsStdioMcpTransport::send(
    const Domain::McpFrame& frame,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->send(frame, context);
}

void WindowsStdioMcpTransport::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Mcp
