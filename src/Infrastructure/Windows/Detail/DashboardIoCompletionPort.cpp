#include "DashboardIoCompletionPort.h"

#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] bool isValidHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// Bridges native creation to the final owner without a double-close window.
// If allocation throws, the guard closes the port. Once the owner exists the
// guard is disarmed, so any later result-construction unwind closes through the
// owner and only through the owner.
class CreatedCompletionPortGuard final {
public:
    CreatedCompletionPortGuard(
        IDashboardIoCompletionPortApi& api,
        const HANDLE port) noexcept
        : api_{api}, port_{port}
    {
    }

    ~CreatedCompletionPortGuard() noexcept
    {
        if (isValidHandle(port_)) {
            static_cast<void>(api_.closeHandle(port_));
        }
    }

    CreatedCompletionPortGuard(const CreatedCompletionPortGuard&) = delete;
    CreatedCompletionPortGuard& operator=(
        const CreatedCompletionPortGuard&) = delete;
    CreatedCompletionPortGuard(CreatedCompletionPortGuard&&) = delete;
    CreatedCompletionPortGuard& operator=(
        CreatedCompletionPortGuard&&) = delete;

    void markOwned() noexcept { port_ = nullptr; }

private:
    IDashboardIoCompletionPortApi& api_;
    HANDLE port_{};
};

[[nodiscard]] Domain::Error nativeError(
    const std::string_view action,
    const DWORD nativeCode,
    const std::string_view stableCode,
    const bool retryable = false) noexcept
{
    try {
        std::string message{action};
        message += " failed with Win32 error ";
        message += std::to_string(nativeCode);
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard I/O completion-port operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error creationError(const DWORD nativeCode) noexcept
{
    if (nativeCode == ERROR_NOT_ENOUGH_MEMORY ||
        nativeCode == ERROR_NO_SYSTEM_RESOURCES) {
        return nativeError(
            "Create the dashboard I/O completion port",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    }
    return nativeError(
        "Create the dashboard I/O completion port",
        nativeCode,
        Domain::ErrorCodes::HostCapabilityUnavailable);
}

[[nodiscard]] Domain::Error associationError(const DWORD nativeCode) noexcept
{
    if (nativeCode == ERROR_ACCESS_DENIED ||
        nativeCode == ERROR_PRIVILEGE_NOT_HELD) {
        return nativeError(
            "Associate a borrowed object with the dashboard I/O completion port",
            nativeCode,
            Domain::ErrorCodes::Unauthorized);
    }
    if (nativeCode == ERROR_INVALID_PARAMETER) {
        return nativeError(
            "Associate a borrowed object with the dashboard I/O completion port",
            nativeCode,
            Domain::ErrorCodes::Conflict);
    }
    if (nativeCode == ERROR_NOT_ENOUGH_MEMORY ||
        nativeCode == ERROR_NO_SYSTEM_RESOURCES) {
        return nativeError(
            "Associate a borrowed object with the dashboard I/O completion port",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    }
    return nativeError(
        "Associate a borrowed object with the dashboard I/O completion port",
        nativeCode,
        Domain::ErrorCodes::InternalFailure);
}

[[nodiscard]] Domain::Error postError(const DWORD nativeCode) noexcept
{
    if (nativeCode == ERROR_INVALID_HANDLE ||
        nativeCode == ERROR_ABANDONED_WAIT_0) {
        return nativeError(
            "Post a dashboard I/O completion packet",
            nativeCode,
            Domain::ErrorCodes::TransportClosed,
            true);
    }
    if (nativeCode == ERROR_NOT_ENOUGH_MEMORY ||
        nativeCode == ERROR_NO_SYSTEM_RESOURCES) {
        return nativeError(
            "Post a dashboard I/O completion packet",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    }
    return nativeError(
        "Post a dashboard I/O completion packet",
        nativeCode,
        Domain::ErrorCodes::InternalFailure);
}

} // namespace

DashboardIoCompletionControlPostResult::
DashboardIoCompletionControlPostResult(
    const bool succeeded,
    const DWORD nativeError) noexcept
    : succeeded_{succeeded}, nativeError_{nativeError}
{
}

DashboardIoCompletionDequeueResult::DashboardIoCompletionDequeueResult(
    const DashboardIoCompletionDequeueDisposition disposition,
    std::optional<DashboardIoCompletionPacket> packet,
    const DWORD nativeError) noexcept
    : disposition_{disposition},
      packet_{std::move(packet)},
      nativeError_{nativeError}
{
}

HANDLE DashboardIoCompletionPortSystemApi::createIoCompletionPort(
    const HANDLE fileHandle,
    const HANDLE existingCompletionPort,
    const ULONG_PTR completionKey,
    const DWORD concurrentThreadCount) noexcept
{
    return ::CreateIoCompletionPort(
        fileHandle,
        existingCompletionPort,
        completionKey,
        concurrentThreadCount);
}

BOOL DashboardIoCompletionPortSystemApi::postQueuedCompletionStatus(
    const HANDLE completionPort,
    const DWORD transferredBytes,
    const ULONG_PTR completionKey,
    OVERLAPPED* const operation) noexcept
{
    return ::PostQueuedCompletionStatus(
        completionPort,
        transferredBytes,
        completionKey,
        operation);
}

BOOL DashboardIoCompletionPortSystemApi::getQueuedCompletionStatus(
    const HANDLE completionPort,
    DWORD& transferredBytes,
    ULONG_PTR& completionKey,
    OVERLAPPED*& operation,
    const DWORD timeoutMilliseconds) noexcept
{
    return ::GetQueuedCompletionStatus(
        completionPort,
        &transferredBytes,
        &completionKey,
        &operation,
        timeoutMilliseconds);
}

BOOL DashboardIoCompletionPortSystemApi::closeHandle(
    const HANDLE handle) noexcept
{
    return ::CloseHandle(handle);
}

DWORD DashboardIoCompletionPortSystemApi::lastError() noexcept
{
    return ::GetLastError();
}

DashboardIoCompletionPort::DashboardIoCompletionPort(
    std::shared_ptr<IDashboardIoCompletionPortApi> api,
    const HANDLE port) noexcept
    : api_{std::move(api)}, port_{port}
{
}

DashboardIoCompletionPort::~DashboardIoCompletionPort() noexcept
{
    if (isValidHandle(port_)) {
        static_cast<void>(api_->closeHandle(port_));
        port_ = nullptr;
    }
}

Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>
DashboardIoCompletionPort::create() noexcept
{
    try {
        return create(std::make_shared<DashboardIoCompletionPortSystemApi>());
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its I/O completion-port API owner."));
    }
}

Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>
DashboardIoCompletionPort::create(
    std::shared_ptr<IDashboardIoCompletionPortApi> api) noexcept
{
    if (api == nullptr) {
        return Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard I/O completion port requires a native API dependency."));
    }

    const HANDLE port = api->createIoCompletionPort(
        INVALID_HANDLE_VALUE,
        nullptr,
        0U,
        RequiredConcurrencyThreadCount);
    if (!isValidHandle(port)) {
        return Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>::failure(
            creationError(api->lastError()));
    }

    try {
        CreatedCompletionPortGuard unwindGuard{*api, port};
        auto owner = std::unique_ptr<DashboardIoCompletionPort>{
            new DashboardIoCompletionPort{api, port}};
        unwindGuard.markOwned();
        return Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>::success(
            std::move(owner));
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardIoCompletionPort>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not own its newly created I/O completion port."));
    }
}

Domain::Result<void> DashboardIoCompletionPort::associateHandle(
    const HANDLE handle,
    const DashboardIoCompletionKey completionKey) const noexcept
{
    if (!isValidHandle(handle)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard I/O completion port cannot associate an invalid borrowed handle."));
    }

    const HANDLE associated = api_->createIoCompletionPort(
        handle,
        port_,
        static_cast<ULONG_PTR>(completionKey.value()),
        0U);
    if (associated == nullptr) {
        return Domain::Result<void>::failure(
            associationError(api_->lastError()));
    }
    if (associated != port_) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Windows returned a different I/O completion port while associating a borrowed handle."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<void> DashboardIoCompletionPort::associateSocket(
    const SOCKET socket,
    const DashboardIoCompletionKey completionKey) const noexcept
{
    if (socket == INVALID_SOCKET) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard I/O completion port cannot associate an invalid borrowed socket."));
    }
    return associateHandle(
        reinterpret_cast<HANDLE>(socket),
        completionKey);
}

Domain::Result<void> DashboardIoCompletionPort::post(
    const DWORD transferredBytes,
    const DashboardIoCompletionKey completionKey,
    OVERLAPPED* const operation) const noexcept
{
    if (api_->postQueuedCompletionStatus(
            port_,
            transferredBytes,
            static_cast<ULONG_PTR>(completionKey.value()),
            operation) == FALSE) {
        return Domain::Result<void>::failure(postError(api_->lastError()));
    }
    return Domain::Result<void>::success();
}

DashboardIoCompletionControlPostResult
DashboardIoCompletionPort::postControl(
    const DashboardIoCompletionKey completionKey) const noexcept
{
    if (api_->postQueuedCompletionStatus(
            port_,
            0U,
            static_cast<ULONG_PTR>(completionKey.value()),
            nullptr) == FALSE) {
        return DashboardIoCompletionControlPostResult{
            false, api_->lastError()};
    }
    return DashboardIoCompletionControlPostResult{true, ERROR_SUCCESS};
}

DashboardIoCompletionDequeueResult DashboardIoCompletionPort::dequeue(
    const DWORD timeoutMilliseconds) const noexcept
{
    DWORD transferredBytes = 0U;
    ULONG_PTR completionKey = 0U;
    OVERLAPPED* operation = nullptr;
    if (api_->getQueuedCompletionStatus(
            port_,
            transferredBytes,
            completionKey,
            operation,
            timeoutMilliseconds) != FALSE) {
        return DashboardIoCompletionDequeueResult{
            DashboardIoCompletionDequeueDisposition::Succeeded,
            DashboardIoCompletionPacket{
                transferredBytes,
                DashboardIoCompletionKey{
                    static_cast<std::uintptr_t>(completionKey)},
                operation},
            ERROR_SUCCESS};
    }

    const DWORD error = api_->lastError();
    if (operation != nullptr) {
        return DashboardIoCompletionDequeueResult{
            DashboardIoCompletionDequeueDisposition::IoFailed,
            DashboardIoCompletionPacket{
                transferredBytes,
                DashboardIoCompletionKey{
                    static_cast<std::uintptr_t>(completionKey)},
                operation},
            error};
    }
    if (error == WAIT_TIMEOUT) {
        return DashboardIoCompletionDequeueResult{
            DashboardIoCompletionDequeueDisposition::TimedOut,
            std::nullopt,
            WAIT_TIMEOUT};
    }
    return DashboardIoCompletionDequeueResult{
        DashboardIoCompletionDequeueDisposition::FatalError,
        std::nullopt,
        error};
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
