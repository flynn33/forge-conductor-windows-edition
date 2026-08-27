#include "ManagerPipeIo.h"

#include "Win32Error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr DWORD FramePrefixBytes = 4U;
constexpr DWORD OpenRetryMilliseconds = 20U;
constexpr DWORD NativeIoReapMilliseconds = 5'000U;
constexpr std::wstring_view LocalPipePrefix = L"\\\\.\\pipe\\";
constexpr std::array<std::byte, FramePrefixBytes +
    ManagerPipeResponseReceiptPayloadBytes> ResponseReceiptFrame{
    std::byte{4U}, std::byte{0U}, std::byte{0U}, std::byte{0U},
    std::byte{'F'}, std::byte{'C'}, std::byte{'R'}, std::byte{'1'}};

enum class WaitOutcome {
    completed,
    shutdown,
    cancelled,
    deadline,
    failed,
};

struct WaitResult final {
    WaitOutcome outcome{WaitOutcome::failed};
    DWORD nativeError{ERROR_SUCCESS};
};

class OperationWaitState final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<OperationWaitState>>
    create(const std::stop_token cancellation) noexcept
    {
        try {
            UniqueHandle event{
                ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!event) {
                return Domain::Result<std::unique_ptr<OperationWaitState>>::failure(
                    makeWin32Error(
                        "create the manager pipe cancellation event",
                        ::GetLastError()));
            }
            return Domain::Result<std::unique_ptr<OperationWaitState>>::success(
                std::unique_ptr<OperationWaitState>{
                    new OperationWaitState{std::move(event), cancellation}});
        } catch (...) {
            return Domain::Result<std::unique_ptr<OperationWaitState>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager pipe cancellation state could not be allocated."));
        }
    }

    ~OperationWaitState() noexcept = default;
    OperationWaitState(const OperationWaitState&) = delete;
    OperationWaitState& operator=(const OperationWaitState&) = delete;
    OperationWaitState(OperationWaitState&&) = delete;
    OperationWaitState& operator=(OperationWaitState&&) = delete;

    [[nodiscard]] HANDLE cancellationEvent() const noexcept
    {
        return cancellationEvent_.get();
    }

private:
    struct CancellationRelay final {
        HANDLE event{};

        void operator()() const noexcept
        {
            if (::SetEvent(event) == FALSE) {
                // A pending native I/O request must always have a reliable
                // stop-token wake path.
                std::terminate();
            }
        }
    };

    OperationWaitState(
        UniqueHandle cancellationEvent,
        const std::stop_token cancellation) noexcept
        : cancellationEvent_{std::move(cancellationEvent)},
          cancellationCallback_{
              cancellation, CancellationRelay{cancellationEvent_.get()}}
    {
    }

    UniqueHandle cancellationEvent_;
    std::stop_callback<CancellationRelay> cancellationCallback_;
};

[[nodiscard]] bool isValidHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool isPipeTerminationError(const DWORD error) noexcept
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_NO_DATA || error == ERROR_HANDLE_EOF ||
        error == ERROR_OPERATION_ABORTED;
}

[[nodiscard]] bool isOpenRetryError(const DWORD error) noexcept
{
    return error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND ||
        error == ERROR_PATH_NOT_FOUND || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_SEM_TIMEOUT;
}

[[nodiscard]] Domain::Error invalidHandleError(
    const std::string_view description) noexcept
{
    try {
        return Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{description} + " received an invalid Windows handle.");
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A manager pipe handle validation failed.");
    }
}

[[nodiscard]] Domain::Error pipeError(
    const std::string_view action,
    const DWORD nativeError) noexcept
{
    if (isPipeTerminationError(nativeError)) {
        return makeWin32Error(
            action, nativeError, Domain::ErrorCodes::TransportClosed, true);
    }
    if (nativeError == ERROR_ACCESS_DENIED || nativeError == ERROR_PRIVILEGE_NOT_HELD) {
        return makeWin32Error(
            action, nativeError, Domain::ErrorCodes::Unauthorized);
    }
    return makeWin32Error(action, nativeError);
}

[[nodiscard]] Domain::Error cancelledError(
    const bool shutdown) noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        shutdown ? "The manager pipe operation was stopped by manager shutdown." :
                   "The manager pipe operation was cancelled.");
}

[[nodiscard]] Domain::Error deadlineError() noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "The manager pipe operation exceeded its deadline.", true);
}

[[nodiscard]] Domain::Result<void> validateMaximumPayload(
    const std::size_t maximumPayloadBytes) noexcept
{
    if (maximumPayloadBytes > DefaultManagerPipeMaximumPayloadBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The manager pipe payload bound exceeded the two-mebibyte transport ceiling."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> preflight(
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent) noexcept
{
    if (!isValidHandle(shutdownEvent)) {
        return Domain::Result<void>::failure(
            invalidHandleError("The manager pipe shutdown event"));
    }
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(cancelledError(false));
    }

    const DWORD shutdownStatus = ::WaitForSingleObject(shutdownEvent, 0U);
    if (shutdownStatus == WAIT_OBJECT_0) {
        return Domain::Result<void>::failure(cancelledError(true));
    }
    if (shutdownStatus != WAIT_TIMEOUT) {
        const DWORD nativeError = shutdownStatus == WAIT_FAILED ?
            ::GetLastError() : ERROR_INVALID_HANDLE;
        return Domain::Result<void>::failure(makeWin32Error(
            "inspect the manager pipe shutdown event", nativeError,
            Domain::ErrorCodes::InvalidRequest));
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(deadlineError());
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] DWORD remainingDeadlineMilliseconds(
    const Domain::OperationContext& context) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (context.deadline <= now) {
        return 0U;
    }

    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        context.deadline - now);
    constexpr DWORD MaximumFiniteWait =
        (std::numeric_limits<DWORD>::max)() - 1U;
    const auto maximumFiniteWait = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(MaximumFiniteWait)};
    if (remaining >= maximumFiniteWait) {
        return MaximumFiniteWait;
    }
    return static_cast<DWORD>(remaining.count());
}

[[nodiscard]] WaitResult waitForOperation(
    const HANDLE operationEvent,
    const HANDLE shutdownEvent,
    const HANDLE cancellationEvent,
    const Domain::OperationContext& context) noexcept
{
    const std::array<HANDLE, 3U> handles{
        operationEvent, shutdownEvent, cancellationEvent};
    const DWORD waited = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        remainingDeadlineMilliseconds(context));
    if (waited == WAIT_OBJECT_0) {
        return WaitResult{WaitOutcome::completed, ERROR_SUCCESS};
    }
    if (waited == WAIT_OBJECT_0 + 1U) {
        return WaitResult{WaitOutcome::shutdown, ERROR_SUCCESS};
    }
    if (waited == WAIT_OBJECT_0 + 2U) {
        return WaitResult{WaitOutcome::cancelled, ERROR_SUCCESS};
    }
    if (waited == WAIT_TIMEOUT) {
        return WaitResult{WaitOutcome::deadline, ERROR_SUCCESS};
    }
    return WaitResult{
        WaitOutcome::failed,
        waited == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE};
}

[[nodiscard]] Domain::Result<void> cancelAndReap(
    const HANDLE pipe,
    OVERLAPPED& operation,
    const std::string_view action) noexcept
{
    const BOOL cancelled = ::CancelIoEx(pipe, &operation);
    const DWORD cancellationError = cancelled != FALSE ?
        ERROR_SUCCESS : ::GetLastError();

    const DWORD waited = ::WaitForSingleObject(
        operation.hEvent, NativeIoReapMilliseconds);
    if (waited != WAIT_OBJECT_0) {
        // Returning would release stack-owned OVERLAPPED storage while the
        // kernel might still reference it.
        std::terminate();
    }

    DWORD ignoredBytes = 0U;
    if (::GetOverlappedResult(pipe, &operation, &ignoredBytes, FALSE) == FALSE) {
        const DWORD completionError = ::GetLastError();
        if (completionError == ERROR_IO_INCOMPLETE) {
            std::terminate();
        }
        if (!isPipeTerminationError(completionError)) {
            return Domain::Result<void>::failure(pipeError(
                "reap a cancelled manager pipe operation", completionError));
        }
    }
    if (cancelled == FALSE && cancellationError != ERROR_NOT_FOUND) {
        return Domain::Result<void>::failure(makeWin32Error(
            action, cancellationError));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<DWORD> completePendingOperation(
    const HANDLE pipe,
    OVERLAPPED& operation,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const HANDLE cancellationEvent,
    const std::string_view action,
    const bool connectOperation = false) noexcept
{
    const WaitResult waited = waitForOperation(
        operation.hEvent, shutdownEvent, cancellationEvent, context);
    if (waited.outcome == WaitOutcome::completed) {
        DWORD transferred = 0U;
        if (::GetOverlappedResult(
                pipe, &operation, &transferred, FALSE) != FALSE) {
            return Domain::Result<DWORD>::success(transferred);
        }
        const DWORD completionError = ::GetLastError();
        if (completionError == ERROR_IO_INCOMPLETE) {
            std::terminate();
        }
        if (connectOperation && completionError == ERROR_PIPE_CONNECTED) {
            return Domain::Result<DWORD>::success(0U);
        }
        return Domain::Result<DWORD>::failure(
            pipeError(action, completionError));
    }

    auto reaped = cancelAndReap(
        pipe, operation, "cancel a pending manager pipe operation");
    if (!reaped) {
        return Domain::Result<DWORD>::failure(std::move(reaped).error());
    }

    switch (waited.outcome) {
    case WaitOutcome::shutdown:
        return Domain::Result<DWORD>::failure(cancelledError(true));
    case WaitOutcome::cancelled:
        return Domain::Result<DWORD>::failure(cancelledError(false));
    case WaitOutcome::deadline:
        return Domain::Result<DWORD>::failure(deadlineError());
    case WaitOutcome::failed:
        return Domain::Result<DWORD>::failure(makeWin32Error(
            "wait for a pending manager pipe operation",
            waited.nativeError));
    case WaitOutcome::completed:
        break;
    }
    return Domain::Result<DWORD>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The manager pipe operation reached an invalid wait state."));
}

[[nodiscard]] Domain::Result<void> readExact(
    const HANDLE pipe,
    const std::span<std::byte> destination,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const HANDLE cancellationEvent) noexcept
{
    std::size_t offset = 0U;
    while (offset < destination.size()) {
        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return ready;
        }

        UniqueHandle completedEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!completedEvent) {
            return Domain::Result<void>::failure(makeWin32Error(
                "create a manager pipe read completion event",
                ::GetLastError()));
        }
        OVERLAPPED operation{};
        operation.hEvent = completedEvent.get();

        const std::size_t remaining = destination.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD transferred = 0U;
        const BOOL completed = ::ReadFile(
            pipe, destination.data() + offset, requested, &transferred,
            &operation);
        if (completed == FALSE) {
            const DWORD startError = ::GetLastError();
            if (startError != ERROR_IO_PENDING) {
                return Domain::Result<void>::failure(
                    pipeError("read a manager pipe frame", startError));
            }
            auto pending = completePendingOperation(
                pipe, operation, context, shutdownEvent, cancellationEvent,
                "complete a manager pipe frame read");
            if (!pending) {
                return Domain::Result<void>::failure(std::move(pending).error());
            }
            transferred = pending.value();
        }
        if (transferred == 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The manager pipe peer closed before the complete frame was read.",
                true));
        }
        if (transferred > requested) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Windows reported an impossible manager pipe read length."));
        }
        offset += transferred;
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> writeExact(
    const HANDLE pipe,
    const std::span<const std::byte> source,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const HANDLE cancellationEvent) noexcept
{
    std::size_t offset = 0U;
    while (offset < source.size()) {
        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return ready;
        }

        UniqueHandle completedEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!completedEvent) {
            return Domain::Result<void>::failure(makeWin32Error(
                "create a manager pipe write completion event",
                ::GetLastError()));
        }
        OVERLAPPED operation{};
        operation.hEvent = completedEvent.get();

        const std::size_t remaining = source.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD transferred = 0U;
        const BOOL completed = ::WriteFile(
            pipe, source.data() + offset, requested, &transferred,
            &operation);
        if (completed == FALSE) {
            const DWORD startError = ::GetLastError();
            if (startError != ERROR_IO_PENDING) {
                return Domain::Result<void>::failure(
                    pipeError("write a manager pipe frame", startError));
            }
            auto pending = completePendingOperation(
                pipe, operation, context, shutdownEvent, cancellationEvent,
                "complete a manager pipe frame write");
            if (!pending) {
                return Domain::Result<void>::failure(std::move(pending).error());
            }
            transferred = pending.value();
        }
        if (transferred == 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The manager pipe peer closed before the complete frame was written.",
                true));
        }
        if (transferred > requested) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Windows reported an impossible manager pipe write length."));
        }
        offset += transferred;
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::uint32_t decodeFrameLength(
    const std::span<const std::byte, FramePrefixBytes> prefix) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(prefix[0])) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(prefix[1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(prefix[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(prefix[3])) << 24U);
}

[[nodiscard]] Domain::Result<void> waitBeforeOpenRetry(
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const HANDLE cancellationEvent) noexcept
{
    const DWORD remaining = remainingDeadlineMilliseconds(context);
    const DWORD waitMilliseconds = (std::min)(remaining, OpenRetryMilliseconds);
    const std::array<HANDLE, 2U> handles{shutdownEvent, cancellationEvent};
    const DWORD waited = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        waitMilliseconds);
    if (waited == WAIT_OBJECT_0) {
        return Domain::Result<void>::failure(cancelledError(true));
    }
    if (waited == WAIT_OBJECT_0 + 1U) {
        return Domain::Result<void>::failure(cancelledError(false));
    }
    if (waited == WAIT_TIMEOUT) {
        return Domain::Result<void>::success();
    }
    const DWORD nativeError = waited == WAIT_FAILED ?
        ::GetLastError() : ERROR_GEN_FAILURE;
    return Domain::Result<void>::failure(makeWin32Error(
        "wait before retrying the manager pipe open", nativeError));
}

} // namespace

Domain::Result<UniqueHandle> openManagerPipe(
    const std::wstring_view pipeName,
    const DWORD desiredAccess,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent) noexcept
{
    try {
        if (pipeName.empty() ||
            pipeName.size() > MaximumManagerPipeNameCharacters ||
            !pipeName.starts_with(LocalPipePrefix) ||
            pipeName.find(L'\0') != std::wstring_view::npos) {
            return Domain::Result<UniqueHandle>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager pipe name was not a bounded local pipe path."));
        }
        constexpr DWORD AllowedDesiredAccess = GENERIC_READ | GENERIC_WRITE;
        if (desiredAccess == 0U ||
            (desiredAccess & ~AllowedDesiredAccess) != 0U) {
            return Domain::Result<UniqueHandle>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager pipe client requested unsupported access rights."));
        }
        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return Domain::Result<UniqueHandle>::failure(
                std::move(ready).error());
        }
        auto waitStateResult = OperationWaitState::create(context.cancellation);
        if (!waitStateResult) {
            return Domain::Result<UniqueHandle>::failure(
                std::move(waitStateResult).error());
        }
        auto waitState = std::move(waitStateResult).value();
        const std::wstring ownedName{pipeName};

        for (;;) {
            ready = preflight(context, shutdownEvent);
            if (!ready) {
                return Domain::Result<UniqueHandle>::failure(
                    std::move(ready).error());
            }

            UniqueHandle pipe{::CreateFileW(
                ownedName.c_str(), desiredAccess, 0U, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                    SECURITY_IDENTIFICATION,
                nullptr)};
            if (pipe) {
                if (::SetHandleInformation(
                        pipe.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE) {
                    return Domain::Result<UniqueHandle>::failure(makeWin32Error(
                        "make the manager pipe client handle non-inheritable",
                        ::GetLastError()));
                }
                ready = preflight(context, shutdownEvent);
                if (!ready) {
                    return Domain::Result<UniqueHandle>::failure(
                        std::move(ready).error());
                }
                return Domain::Result<UniqueHandle>::success(std::move(pipe));
            }

            const DWORD openError = ::GetLastError();
            if (!isOpenRetryError(openError)) {
                return Domain::Result<UniqueHandle>::failure(
                    pipeError("open the manager named pipe", openError));
            }
            auto waited = waitBeforeOpenRetry(
                context, shutdownEvent, waitState->cancellationEvent());
            if (!waited) {
                return Domain::Result<UniqueHandle>::failure(
                    std::move(waited).error());
            }
        }
    } catch (...) {
        return Domain::Result<UniqueHandle>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager pipe client could not allocate bounded open state."));
    }
}

Domain::Result<void> connectManagerPipe(
    const HANDLE serverPipe,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent) noexcept
{
    try {
        if (!isValidHandle(serverPipe)) {
            return Domain::Result<void>::failure(
                invalidHandleError("The manager pipe server connection"));
        }
        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return ready;
        }
        auto waitStateResult = OperationWaitState::create(context.cancellation);
        if (!waitStateResult) {
            return Domain::Result<void>::failure(
                std::move(waitStateResult).error());
        }
        auto waitState = std::move(waitStateResult).value();
        UniqueHandle completedEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!completedEvent) {
            return Domain::Result<void>::failure(makeWin32Error(
                "create a manager pipe connection completion event",
                ::GetLastError()));
        }

        OVERLAPPED operation{};
        operation.hEvent = completedEvent.get();
        if (::ConnectNamedPipe(serverPipe, &operation) != FALSE) {
            return Domain::Result<void>::success();
        }
        const DWORD startError = ::GetLastError();
        if (startError == ERROR_PIPE_CONNECTED) {
            return Domain::Result<void>::success();
        }
        if (startError != ERROR_IO_PENDING) {
            return Domain::Result<void>::failure(
                pipeError("connect the manager named pipe", startError));
        }

        auto completed = completePendingOperation(
            serverPipe, operation, context, shutdownEvent,
            waitState->cancellationEvent(),
            "complete the manager named-pipe connection", true);
        if (!completed) {
            return Domain::Result<void>::failure(std::move(completed).error());
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager pipe server could not allocate bounded connection state."));
    }
}

Domain::Result<std::vector<std::byte>> readManagerPipeFrame(
    const HANDLE pipe,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const std::size_t maximumPayloadBytes) noexcept
{
    try {
        if (!isValidHandle(pipe)) {
            return Domain::Result<std::vector<std::byte>>::failure(
                invalidHandleError("The manager pipe frame reader"));
        }
        auto validMaximum = validateMaximumPayload(maximumPayloadBytes);
        if (!validMaximum) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(validMaximum).error());
        }
        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(ready).error());
        }
        auto waitStateResult = OperationWaitState::create(context.cancellation);
        if (!waitStateResult) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(waitStateResult).error());
        }
        auto waitState = std::move(waitStateResult).value();

        std::array<std::byte, FramePrefixBytes> prefix{};
        auto prefixRead = readExact(
            pipe, prefix, context, shutdownEvent,
            waitState->cancellationEvent());
        if (!prefixRead) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(prefixRead).error());
        }
        const std::uint32_t payloadBytes = decodeFrameLength(prefix);
        if (payloadBytes > maximumPayloadBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The manager pipe frame declared a payload above its configured bound."));
        }

        std::vector<std::byte> frame(
            static_cast<std::size_t>(FramePrefixBytes) + payloadBytes);
        std::ranges::copy(prefix, frame.begin());
        if (payloadBytes != 0U) {
            auto payloadRead = readExact(
                pipe,
                std::span<std::byte>{frame}.subspan(FramePrefixBytes),
                context, shutdownEvent, waitState->cancellationEvent());
            if (!payloadRead) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(payloadRead).error());
            }
        }
        return Domain::Result<std::vector<std::byte>>::success(
            std::move(frame));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The manager pipe frame reader could not allocate bounded state."));
    }
}

Domain::Result<void> writeManagerPipeFrame(
    const HANDLE pipe,
    const std::span<const std::byte> frame,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent,
    const std::size_t maximumPayloadBytes) noexcept
{
    try {
        if (!isValidHandle(pipe)) {
            return Domain::Result<void>::failure(
                invalidHandleError("The manager pipe frame writer"));
        }
        auto validMaximum = validateMaximumPayload(maximumPayloadBytes);
        if (!validMaximum) {
            return validMaximum;
        }
        if (frame.size() < FramePrefixBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::MalformedMessage,
                "The manager pipe frame did not contain its four-byte length prefix."));
        }

        const auto prefix = std::span<const std::byte, FramePrefixBytes>{
            frame.data(), FramePrefixBytes};
        const std::uint32_t payloadBytes = decodeFrameLength(prefix);
        if (payloadBytes > maximumPayloadBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The manager pipe frame declared a payload above its configured bound."));
        }
        if (frame.size() - FramePrefixBytes != payloadBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::MalformedMessage,
                "The manager pipe frame length prefix did not match its exact payload size."));
        }

        auto ready = preflight(context, shutdownEvent);
        if (!ready) {
            return ready;
        }
        auto waitStateResult = OperationWaitState::create(context.cancellation);
        if (!waitStateResult) {
            return Domain::Result<void>::failure(
                std::move(waitStateResult).error());
        }
        auto waitState = std::move(waitStateResult).value();
        return writeExact(
            pipe, frame, context, shutdownEvent,
            waitState->cancellationEvent());
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager pipe frame writer could not allocate bounded state."));
    }
}

Domain::Result<void> writeManagerPipeResponseReceipt(
    const HANDLE pipe,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent) noexcept
{
    return writeManagerPipeFrame(
        pipe,
        ResponseReceiptFrame,
        context,
        shutdownEvent,
        ManagerPipeResponseReceiptPayloadBytes);
}

Domain::Result<void> readManagerPipeResponseReceipt(
    const HANDLE pipe,
    const Domain::OperationContext& context,
    const HANDLE shutdownEvent) noexcept
{
    auto frame = readManagerPipeFrame(
        pipe,
        context,
        shutdownEvent,
        ManagerPipeResponseReceiptPayloadBytes);
    if (!frame) {
        return Domain::Result<void>::failure(std::move(frame).error());
    }
    if (!std::ranges::equal(frame.value(), ResponseReceiptFrame)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            "The manager pipe response receipt was invalid."));
    }
    return Domain::Result<void>::success();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
