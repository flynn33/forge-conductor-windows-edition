#include "OverlappedPipeReader.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr ULONG_PTR ShutdownCompletionKey = (std::numeric_limits<ULONG_PTR>::max)();
constexpr ULONG_PTR PipeCompletionKey = 1U;
constexpr std::chrono::milliseconds NativeIoReapTimeout{2'000};
constexpr DWORD NativeIoReapTimeoutMilliseconds = 2'000U;

[[nodiscard]] bool isExpectedPipeTermination(const DWORD error) noexcept
{
    return error == ERROR_SUCCESS || error == ERROR_BROKEN_PIPE ||
           error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA ||
           error == ERROR_HANDLE_EOF || error == ERROR_OPERATION_ABORTED;
}

[[nodiscard]] DWORD cancelPendingRead(const HANDLE handle, OVERLAPPED* const overlapped) noexcept
{
    if (::CancelIoEx(handle, overlapped)) {
        return ERROR_SUCCESS;
    }
    const auto error = ::GetLastError();
    return error == ERROR_NOT_FOUND ? ERROR_SUCCESS : error;
}

[[nodiscard]] Domain::Error win32Failure(const std::string_view action, const DWORD error)
{
    return Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{action} +
                                                                      " failed with Win32 error " +
                                                                      std::to_string(error) + ".");
}

[[nodiscard]] Domain::Result<void> cancelAndReapConnect(const HANDLE server, OVERLAPPED& connection)
{
    const auto cancelled = ::CancelIoEx(server, &connection);
    const auto cancelError = cancelled ? ERROR_SUCCESS : ::GetLastError();

    // The OVERLAPPED and its event are stack-owned by createConnectedWriter. They may only leave
    // scope after the event proves that the kernel has completed the request. If the bounded wait
    // cannot establish that invariant, continuing would be a use-after-free, so fail closed.
    if (::WaitForSingleObject(connection.hEvent, NativeIoReapTimeoutMilliseconds) !=
        WAIT_OBJECT_0) {
        std::terminate();
    }

    DWORD ignored{};
    if (!::GetOverlappedResult(server, &connection, &ignored, FALSE)) {
        const auto completionError = ::GetLastError();
        if (completionError == ERROR_IO_INCOMPLETE) {
            std::terminate();
        }
        if (completionError != ERROR_OPERATION_ABORTED && completionError != ERROR_PIPE_CONNECTED) {
            return Domain::Result<void>::failure(win32Failure(
                "GetOverlappedResult while reaping process-pipe connection", completionError));
        }
    }
    if (!cancelled && cancelError != ERROR_NOT_FOUND) {
        return Domain::Result<void>::failure(
            win32Failure("CancelIoEx for process-pipe connection", cancelError));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<UniqueHandle> createConnectedWriter(const HANDLE server,
                                                                 const std::wstring& pipeName)
{
    UniqueHandle connectedEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!connectedEvent) {
        return Domain::Result<UniqueHandle>::failure(
            win32Failure("CreateEventW", ::GetLastError()));
    }

    OVERLAPPED connection{};
    connection.hEvent = connectedEvent.get();
    const auto connectStarted = ::ConnectNamedPipe(server, &connection);
    const auto connectError = connectStarted ? ERROR_SUCCESS : ::GetLastError();
    bool connectPending{};
    if (!connectStarted) {
        if (connectError == ERROR_IO_PENDING) {
            connectPending = true;
        } else if (connectError != ERROR_PIPE_CONNECTED) {
            return Domain::Result<UniqueHandle>::failure(
                win32Failure("ConnectNamedPipe", connectError));
        }
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    UniqueHandle writer{::CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, &inheritable,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!writer) {
        const auto writerError = ::GetLastError();
        if (connectPending) {
            auto reaped = cancelAndReapConnect(server, connection);
            if (!reaped) {
                return Domain::Result<UniqueHandle>::failure(std::move(reaped).error());
            }
        }
        return Domain::Result<UniqueHandle>::failure(
            win32Failure("CreateFileW for child pipe writer", writerError));
    }

    if (connectPending) {
        const auto waitResult = ::WaitForSingleObject(connectedEvent.get(), 5'000U);
        if (waitResult != WAIT_OBJECT_0) {
            const auto waitError = waitResult == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
            writer.reset();
            auto reaped = cancelAndReapConnect(server, connection);
            if (!reaped) {
                return Domain::Result<UniqueHandle>::failure(std::move(reaped).error());
            }
            if (waitResult == WAIT_TIMEOUT) {
                return Domain::Result<UniqueHandle>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The local process pipe did not connect within five seconds."));
            }
            return Domain::Result<UniqueHandle>::failure(
                win32Failure("WaitForSingleObject for process-pipe connection", waitError));
        }
        DWORD ignored{};
        if (!::GetOverlappedResult(server, &connection, &ignored, FALSE)) {
            const auto completionError = ::GetLastError();
            if (completionError == ERROR_IO_INCOMPLETE) {
                std::terminate();
            }
            if (completionError != ERROR_PIPE_CONNECTED) {
                return Domain::Result<UniqueHandle>::failure(
                    win32Failure("GetOverlappedResult for process pipe", completionError));
            }
        }
    }

    return Domain::Result<UniqueHandle>::success(std::move(writer));
}

} // namespace

void JobCompletionState::markActiveProcessZero() noexcept
{
    try {
        {
            std::scoped_lock lock{mutex_};
            activeProcessZero_ = true;
        }
        condition_.notify_all();
    } catch (...) {
    }
}

bool JobCompletionState::waitUntilEmpty(const std::chrono::milliseconds timeout) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return activeProcessZero_; });
    } catch (...) {
        return false;
    }
}

JobCompletionRegistration::JobCompletionRegistration(
    IoCompletionPort& completionPort, const ULONG_PTR completionKey,
    std::shared_ptr<JobCompletionState> state) noexcept
    : completionPort_{&completionPort}, completionKey_{completionKey}, state_{std::move(state)}
{
}

JobCompletionRegistration::~JobCompletionRegistration() noexcept
{
    if (completionPort_ != nullptr) {
        completionPort_->unregisterJob(completionKey_, state_.get());
    }
}

bool JobCompletionRegistration::waitUntilEmpty(const std::chrono::milliseconds timeout) noexcept
{
    return state_->waitUntilEmpty(timeout);
}

IoCompletionPort::IoCompletionPort(UniqueHandle port)
    : port_{std::move(port)}, worker_{[this] { workerMain(); }}
{
}

Domain::Result<std::unique_ptr<IoCompletionPort>> IoCompletionPort::create()
{
    UniqueHandle port{::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)};
    if (!port) {
        return Domain::Result<std::unique_ptr<IoCompletionPort>>::failure(
            win32Failure("CreateIoCompletionPort", ::GetLastError()));
    }
    return Domain::Result<std::unique_ptr<IoCompletionPort>>::success(
        std::unique_ptr<IoCompletionPort>{new IoCompletionPort{std::move(port)}});
}

IoCompletionPort::~IoCompletionPort() noexcept { shutdown(); }

Domain::Result<void> IoCompletionPort::associate(const HANDLE handle) const
{
    if (::CreateIoCompletionPort(handle, port_.get(), PipeCompletionKey, 0) == nullptr) {
        return Domain::Result<void>::failure(
            win32Failure("CreateIoCompletionPort association", ::GetLastError()));
    }
    return Domain::Result<void>::success();
}

Domain::Result<std::unique_ptr<JobCompletionRegistration>>
IoCompletionPort::associateJob(const HANDLE job)
{
    auto state = std::make_shared<JobCompletionState>();
    const auto completionKey = nextJobCompletionKey_.fetch_add(1U, std::memory_order_relaxed);
    {
        std::scoped_lock lock{registryMutex_};
        if (shuttingDown_ || completionKey < 2U || completionKey == ShutdownCompletionKey ||
            jobCompletions_.contains(completionKey)) {
            return Domain::Result<std::unique_ptr<JobCompletionRegistration>>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "The process Job Object completion "
                                  "registry rejected an association."));
        }
        jobCompletions_.emplace(completionKey, state);
    }

    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = reinterpret_cast<void*>(completionKey);
    association.CompletionPort = port_.get();
    if (!::SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &association,
                                   sizeof(association))) {
        const auto error = ::GetLastError();
        unregisterJob(completionKey, state.get());
        return Domain::Result<std::unique_ptr<JobCompletionRegistration>>::failure(
            win32Failure("SetInformationJobObject completion-port association", error));
    }

    return Domain::Result<std::unique_ptr<JobCompletionRegistration>>::success(
        std::unique_ptr<JobCompletionRegistration>{
            new JobCompletionRegistration{*this, completionKey, std::move(state)}});
}

void IoCompletionPort::unregisterJob(const ULONG_PTR completionKey,
                                     const JobCompletionState* const state) noexcept
{
    try {
        std::scoped_lock lock{registryMutex_};
        const auto match = jobCompletions_.find(completionKey);
        if (match != jobCompletions_.end() && match->second.get() == state) {
            jobCompletions_.erase(match);
        }
    } catch (...) {
    }
}

Domain::Result<void>
IoCompletionPort::registerPending(OVERLAPPED* const overlapped,
                                  const std::shared_ptr<OverlappedPipeReader>& reader)
{
    std::scoped_lock lock{registryMutex_};
    if (shuttingDown_ || pendingReaders_.contains(overlapped)) {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The process I/O completion registry rejected a read."));
    }
    pendingReaders_.emplace(overlapped, reader);
    return Domain::Result<void>::success();
}

void IoCompletionPort::unregisterPending(OVERLAPPED* const overlapped,
                                         const OverlappedPipeReader* const reader) noexcept
{
    try {
        std::scoped_lock lock{registryMutex_};
        const auto match = pendingReaders_.find(overlapped);
        if (match != pendingReaders_.end() && match->second.get() == reader) {
            pendingReaders_.erase(match);
        }
    } catch (...) {
    }
}

void IoCompletionPort::markWorkerExited() noexcept
{
    try {
        {
            std::scoped_lock lock{workerExitMutex_};
            workerExited_ = true;
        }
        workerExitCondition_.notify_all();
    } catch (...) {
    }
}

bool IoCompletionPort::waitForWorkerExit(const std::chrono::milliseconds timeout) noexcept
{
    try {
        std::unique_lock lock{workerExitMutex_};
        return workerExitCondition_.wait_for(lock, timeout, [this] { return workerExited_; });
    } catch (...) {
        return false;
    }
}

void IoCompletionPort::workerMain() noexcept
{
    const auto completionPort = port_.get();
    for (;;) {
        DWORD bytesTransferred{};
        ULONG_PTR completionKey{};
        OVERLAPPED* overlapped{};
        const auto completed = ::GetQueuedCompletionStatus(completionPort, &bytesTransferred,
                                                           &completionKey, &overlapped, INFINITE);
        const auto error = completed ? ERROR_SUCCESS : ::GetLastError();

        if (overlapped == nullptr && completionKey == ShutdownCompletionKey) {
            break;
        }
        if (overlapped == nullptr && !completed &&
            (error == ERROR_ABANDONED_WAIT_0 || error == ERROR_INVALID_HANDLE)) {
            break;
        }
        if (completionKey != PipeCompletionKey) {
            std::shared_ptr<JobCompletionState> jobCompletion;
            try {
                std::scoped_lock lock{registryMutex_};
                const auto match = jobCompletions_.find(completionKey);
                if (match != jobCompletions_.end()) {
                    jobCompletion = match->second;
                }
            } catch (...) {
            }
            if (jobCompletion && bytesTransferred == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
                jobCompletion->markActiveProcessZero();
            }
            continue;
        }
        if (overlapped == nullptr) {
            continue;
        }

        std::shared_ptr<OverlappedPipeReader> reader;
        try {
            std::scoped_lock lock{registryMutex_};
            const auto match = pendingReaders_.find(overlapped);
            if (match != pendingReaders_.end()) {
                reader = std::move(match->second);
                pendingReaders_.erase(match);
            }
        } catch (...) {
        }
        if (reader) {
            reader->onCompletion(bytesTransferred, error);
        }
    }
    markWorkerExited();
}

void IoCompletionPort::shutdown() noexcept
{
    std::vector<std::shared_ptr<OverlappedPipeReader>> pendingReaders;
    {
        std::scoped_lock lock{registryMutex_};
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        pendingReaders.reserve(pendingReaders_.size());
        for (const auto& [overlapped, reader] : pendingReaders_) {
            static_cast<void>(overlapped);
            pendingReaders.push_back(reader);
        }
    }
    for (const auto& reader : pendingReaders) {
        reader->cancelAndWait();
    }

    if (!::PostQueuedCompletionStatus(port_.get(), 0, ShutdownCompletionKey, nullptr)) {
        // Closing the completion port wakes GetQueuedCompletionStatus with an abandoned/invalid
        // handle status. All registered pipe I/O was reaped above, so no OVERLAPPED remains live.
        port_.reset();
    }
    if (!waitForWorkerExit(NativeIoReapTimeout)) {
        // Never join an unacknowledged worker or release storage that it may still reference.
        std::terminate();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::scoped_lock lock{registryMutex_};
        if (!pendingReaders_.empty()) {
            std::terminate();
        }
        jobCompletions_.clear();
    }
    port_.reset();
}

OverlappedPipeReader::OverlappedPipeReader(IoCompletionPort& completionPort,
                                           UniqueHandle readHandle,
                                           const std::size_t maximumCaptureBytes) noexcept
    : completionPort_{completionPort}, readHandle_{std::move(readHandle)},
      maximumCaptureBytes_{maximumCaptureBytes}
{
}

Domain::Result<PipeEndpoints> OverlappedPipeReader::create(IoCompletionPort& completionPort,
                                                           const std::wstring& pipeName,
                                                           const std::size_t maximumCaptureBytes)
{
    UniqueHandle readerHandle{::CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(16U * 1024U), static_cast<DWORD>(16U * 1024U), 0, nullptr)};
    if (!readerHandle) {
        return Domain::Result<PipeEndpoints>::failure(
            win32Failure("CreateNamedPipeW", ::GetLastError()));
    }
    static_cast<void>(::SetHandleInformation(readerHandle.get(), HANDLE_FLAG_INHERIT, 0));

    auto writer = createConnectedWriter(readerHandle.get(), pipeName);
    if (!writer) {
        return Domain::Result<PipeEndpoints>::failure(std::move(writer).error());
    }

    auto associated = completionPort.associate(readerHandle.get());
    if (!associated) {
        return Domain::Result<PipeEndpoints>::failure(std::move(associated).error());
    }

    auto reader = std::shared_ptr<OverlappedPipeReader>{
        new OverlappedPipeReader{completionPort, std::move(readerHandle), maximumCaptureBytes}};
    return Domain::Result<PipeEndpoints>::success(
        PipeEndpoints{std::move(reader), std::move(writer).value()});
}

Domain::Result<void> OverlappedPipeReader::start()
{
    std::unique_lock lock{mutex_};
    if (started_) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict, "The process pipe reader was started more than once."));
    }
    started_ = true;
    return issueReadLocked(lock);
}

Domain::Result<void> OverlappedPipeReader::issueReadLocked(std::unique_lock<std::mutex>& lock)
{
    overlapped_ = {};
    pending_ = true;
    auto registered = completionPort_.registerPending(&overlapped_, shared_from_this());
    if (!registered) {
        pending_ = false;
        return registered;
    }

    const auto handle = readHandle_.get();
    lock.unlock();
    const auto started = ::ReadFile(handle, buffer_.data(), static_cast<DWORD>(buffer_.size()),
                                    nullptr, &overlapped_);
    const auto error = started ? ERROR_SUCCESS : ::GetLastError();
    lock.lock();

    if (!started && error != ERROR_IO_PENDING) {
        completionPort_.unregisterPending(&overlapped_, this);
        pending_ = false;
        if (!isExpectedPipeTermination(error)) {
            recordReadErrorLocked(error);
            idleCondition_.notify_all();
            return Domain::Result<void>::failure(
                win32Failure("ReadFile for process output", error));
        }
        idleCondition_.notify_all();
    }
    return Domain::Result<void>::success();
}

OverlappedPipeReader::PipeAvailability OverlappedPipeReader::availableBytesLocked() const noexcept
{
    DWORD available{};
    if (!::PeekNamedPipe(readHandle_.get(), nullptr, 0, nullptr, &available, nullptr)) {
        const auto error = ::GetLastError();
        if (isExpectedPipeTermination(error)) {
            return {};
        }
        return PipeAvailability{0U, error};
    }
    return PipeAvailability{available, ERROR_SUCCESS};
}

void OverlappedPipeReader::recordReadErrorLocked(const DWORD error) noexcept
{
    if (!isExpectedPipeTermination(error)) {
        if (readError_ == ERROR_SUCCESS) {
            readError_ = error;
        }
        truncated_ = true;
    }
}

void OverlappedPipeReader::markIdleLocked(const DWORD error) noexcept
{
    pending_ = false;
    recordReadErrorLocked(error);
    idleCondition_.notify_all();
}

void OverlappedPipeReader::onCompletion(const DWORD bytesTransferred, const DWORD error) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        pending_ = false;
        if (bytesTransferred > 0U) {
            const auto remaining = maximumCaptureBytes_ > captured_.size()
                                       ? maximumCaptureBytes_ - captured_.size()
                                       : 0U;
            const auto retained = (std::min)(remaining, static_cast<std::size_t>(bytesTransferred));
            captured_.append(buffer_.data(), retained);
            if (retained < static_cast<std::size_t>(bytesTransferred)) {
                truncated_ = true;
            }
        }

        if (stopping_ || error == ERROR_OPERATION_ABORTED || error == ERROR_BROKEN_PIPE) {
            markIdleLocked(error);
            return;
        }
        if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA) {
            markIdleLocked(error);
            return;
        }
        if (finishing_) {
            const auto availability = availableBytesLocked();
            if (availability.error != ERROR_SUCCESS) {
                markIdleLocked(availability.error);
                return;
            }
            if (availability.bytes == 0U) {
                markIdleLocked(ERROR_SUCCESS);
                return;
            }
        }

        auto next = issueReadLocked(lock);
        if (!next) {
            markIdleLocked(readError_);
        }
    } catch (...) {
        std::scoped_lock lock{mutex_};
        truncated_ = true;
        markIdleLocked(ERROR_NOT_ENOUGH_MEMORY);
    }
}

void OverlappedPipeReader::finishAvailable() noexcept
{
    try {
        std::unique_lock lock{mutex_};
        finishing_ = true;
        if (!pending_) {
            const auto availability = availableBytesLocked();
            if (availability.error != ERROR_SUCCESS) {
                markIdleLocked(availability.error);
            } else if (availability.bytes > 0U && !stopping_) {
                auto next = issueReadLocked(lock);
                if (!next) {
                    markIdleLocked(readError_);
                }
            } else {
                idleCondition_.notify_all();
            }
            return;
        }
        const auto availability = availableBytesLocked();
        if (availability.error != ERROR_SUCCESS) {
            recordReadErrorLocked(availability.error);
            stopping_ = true;
        }
        if (availability.error != ERROR_SUCCESS || availability.bytes == 0U || stopping_) {
            const auto handle = readHandle_.get();
            auto* const overlapped = &overlapped_;
            lock.unlock();
            const auto cancelError = cancelPendingRead(handle, overlapped);
            lock.lock();
            recordReadErrorLocked(cancelError);
        }
    } catch (...) {
        stopImmediately();
    }
}

void OverlappedPipeReader::stopImmediately() noexcept
{
    try {
        std::unique_lock lock{mutex_};
        stopping_ = true;
        if (!pending_) {
            idleCondition_.notify_all();
            return;
        }
        const auto handle = readHandle_.get();
        auto* const overlapped = &overlapped_;
        lock.unlock();
        const auto cancelError = cancelPendingRead(handle, overlapped);
        lock.lock();
        recordReadErrorLocked(cancelError);
    } catch (...) {
    }
}

void OverlappedPipeReader::cancelAndWait() noexcept
{
    try {
        std::unique_lock lock{mutex_};
        stopping_ = true;
        if (pending_) {
            const auto handle = readHandle_.get();
            auto* const overlapped = &overlapped_;
            lock.unlock();
            const auto cancelError = cancelPendingRead(handle, overlapped);
            lock.lock();
            recordReadErrorLocked(cancelError);
        }
        if (!idleCondition_.wait_for(lock, NativeIoReapTimeout, [this] { return !pending_; })) {
            // The member OVERLAPPED and pipe handle cannot be released while the kernel or
            // completion worker might still reference them.
            std::terminate();
        }
    } catch (...) {
        // Continuing would permit OVERLAPPED storage to die while the kernel
        // still owns it, so fail closed instead of risking memory corruption.
        std::terminate();
    }
}

bool OverlappedPipeReader::waitUntilIdle(const std::chrono::milliseconds timeout) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        return idleCondition_.wait_for(lock, timeout, [this] { return !pending_; });
    } catch (...) {
        return false;
    }
}

PipeCapture OverlappedPipeReader::capture() const
{
    std::scoped_lock lock{mutex_};
    return PipeCapture{captured_, truncated_, readError_};
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
