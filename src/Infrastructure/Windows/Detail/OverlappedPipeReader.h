#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class OverlappedPipeReader;
class OverlappedPipeReaderTestAccess;
class IoCompletionPort;

class JobCompletionState final {
public:
    void markActiveProcessZero() noexcept;
    [[nodiscard]] bool waitUntilEmpty(std::chrono::milliseconds timeout) noexcept;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool activeProcessZero_{};
};

class JobCompletionRegistration final {
public:
    ~JobCompletionRegistration() noexcept;
    JobCompletionRegistration(const JobCompletionRegistration&) = delete;
    JobCompletionRegistration& operator=(const JobCompletionRegistration&) = delete;
    JobCompletionRegistration(JobCompletionRegistration&&) = delete;
    JobCompletionRegistration& operator=(JobCompletionRegistration&&) = delete;

    [[nodiscard]] bool waitUntilEmpty(std::chrono::milliseconds timeout) noexcept;

private:
    friend class IoCompletionPort;

    JobCompletionRegistration(IoCompletionPort& completionPort, ULONG_PTR completionKey,
                              std::shared_ptr<JobCompletionState> state) noexcept;

    IoCompletionPort* completionPort_{};
    ULONG_PTR completionKey_{};
    std::shared_ptr<JobCompletionState> state_;
};

class IoCompletionPort final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<IoCompletionPort>> create();

    ~IoCompletionPort() noexcept;
    IoCompletionPort(const IoCompletionPort&) = delete;
    IoCompletionPort& operator=(const IoCompletionPort&) = delete;
    IoCompletionPort(IoCompletionPort&&) = delete;
    IoCompletionPort& operator=(IoCompletionPort&&) = delete;

    [[nodiscard]] Domain::Result<void> associate(HANDLE handle) const;
    [[nodiscard]] Domain::Result<std::unique_ptr<JobCompletionRegistration>>
    associateJob(HANDLE job);
    [[nodiscard]] Domain::Result<void>
    registerPending(OVERLAPPED* overlapped, const std::shared_ptr<OverlappedPipeReader>& reader);
    void unregisterPending(OVERLAPPED* overlapped, const OverlappedPipeReader* reader) noexcept;
    void shutdown() noexcept;

private:
    friend class JobCompletionRegistration;

    explicit IoCompletionPort(UniqueHandle port);
    void markWorkerExited() noexcept;
    [[nodiscard]] bool waitForWorkerExit(std::chrono::milliseconds timeout) noexcept;
    void unregisterJob(ULONG_PTR completionKey, const JobCompletionState* state) noexcept;
    void workerMain() noexcept;

    UniqueHandle port_;
    std::mutex registryMutex_;
    std::unordered_map<OVERLAPPED*, std::shared_ptr<OverlappedPipeReader>> pendingReaders_;
    std::unordered_map<ULONG_PTR, std::shared_ptr<JobCompletionState>> jobCompletions_;
    std::atomic<ULONG_PTR> nextJobCompletionKey_{2U};
    std::mutex workerExitMutex_;
    std::condition_variable workerExitCondition_;
    bool workerExited_{};
    std::jthread worker_;
    bool shuttingDown_{};
};

struct PipeCapture final {
    std::string bytes;
    bool truncated{};
    DWORD readError{ERROR_SUCCESS};
};

struct PipeEndpoints final {
    std::shared_ptr<OverlappedPipeReader> reader;
    UniqueHandle childWriter;
};

class OverlappedPipeReader final : public std::enable_shared_from_this<OverlappedPipeReader> {
public:
    [[nodiscard]] static Domain::Result<PipeEndpoints> create(IoCompletionPort& completionPort,
                                                              const std::wstring& pipeName,
                                                              std::size_t maximumCaptureBytes);

    ~OverlappedPipeReader() = default;
    OverlappedPipeReader(const OverlappedPipeReader&) = delete;
    OverlappedPipeReader& operator=(const OverlappedPipeReader&) = delete;
    OverlappedPipeReader(OverlappedPipeReader&&) = delete;
    OverlappedPipeReader& operator=(OverlappedPipeReader&&) = delete;

    [[nodiscard]] Domain::Result<void> start();
    void finishAvailable() noexcept;
    void stopImmediately() noexcept;
    void cancelAndWait() noexcept;
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] PipeCapture capture() const;

private:
    friend class IoCompletionPort;
    friend class OverlappedPipeReaderTestAccess;

    struct PipeAvailability final {
        DWORD bytes{};
        DWORD error{ERROR_SUCCESS};
    };

    OverlappedPipeReader(IoCompletionPort& completionPort, UniqueHandle readHandle,
                         std::size_t maximumCaptureBytes) noexcept;

    [[nodiscard]] Domain::Result<void> issueReadLocked(std::unique_lock<std::mutex>& lock);
    void onCompletion(DWORD bytesTransferred, DWORD error) noexcept;
    [[nodiscard]] PipeAvailability availableBytesLocked() const noexcept;
    void recordReadErrorLocked(DWORD error) noexcept;
    void markIdleLocked(DWORD error) noexcept;

    IoCompletionPort& completionPort_;
    UniqueHandle readHandle_;
    const std::size_t maximumCaptureBytes_;
    mutable std::mutex mutex_;
    std::condition_variable idleCondition_;
    OVERLAPPED overlapped_{};
    std::array<char, 16U * 1024U> buffer_{};
    std::string captured_;
    DWORD readError_{ERROR_SUCCESS};
    bool started_{};
    bool pending_{};
    bool finishing_{};
    bool stopping_{};
    bool truncated_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
