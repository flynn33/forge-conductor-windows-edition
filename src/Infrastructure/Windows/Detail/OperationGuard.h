#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class JobObject;
class OverlappedPipeReader;

enum class TerminationReason { None, Cancelled, ProcessTimeout, ContextDeadline, Shutdown };

class OperationState final {
public:
    [[nodiscard]] static Domain::Result<std::shared_ptr<OperationState>> create();

    ~OperationState() = default;
    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;
    OperationState(OperationState&&) = delete;
    OperationState& operator=(OperationState&&) = delete;

    void setJob(std::shared_ptr<JobObject> job) noexcept;
    void setRuntimeOwnership(Contracts::RuntimeOwnershipLease childProcess,
                             Contracts::RuntimeOwnershipLease stdoutReader,
                             Contracts::RuntimeOwnershipLease stderrReader) noexcept;
    void setReaders(std::shared_ptr<OverlappedPipeReader> stdoutReader,
                    std::shared_ptr<OverlappedPipeReader> stderrReader) noexcept;

    void requestTermination(TerminationReason reason) noexcept;
    [[nodiscard]] Domain::Result<bool> resumePrimaryThread(HANDLE primaryThread) noexcept;
    void finishReadersAfterDirectChildExit() noexcept;
    void stopReadersImmediately() noexcept;
    void releaseResources() noexcept;

    [[nodiscard]] HANDLE cancellationEvent() const noexcept { return cancellationEvent_.get(); }

    [[nodiscard]] TerminationReason reason() const noexcept
    {
        return reason_.load(std::memory_order_acquire);
    }

private:
    explicit OperationState(UniqueHandle cancellationEvent) noexcept
        : cancellationEvent_{std::move(cancellationEvent)}
    {
    }

    UniqueHandle cancellationEvent_;
    mutable std::mutex mutex_;
    std::optional<Contracts::RuntimeOwnershipLease> childProcessOwnership_;
    std::optional<Contracts::RuntimeOwnershipLease> stdoutReaderOwnership_;
    std::optional<Contracts::RuntimeOwnershipLease> stderrReaderOwnership_;
    std::shared_ptr<JobObject> job_;
    std::shared_ptr<OverlappedPipeReader> stdoutReader_;
    std::shared_ptr<OverlappedPipeReader> stderrReader_;
    std::atomic<TerminationReason> reason_{TerminationReason::None};
};

class OperationRegistry;

class OperationGuard final {
public:
    ~OperationGuard() noexcept;

    OperationGuard(const OperationGuard&) = delete;
    OperationGuard& operator=(const OperationGuard&) = delete;

    OperationGuard(OperationGuard&& other) noexcept;
    OperationGuard& operator=(OperationGuard&& other) noexcept;

    [[nodiscard]] const std::shared_ptr<OperationState>& state() const noexcept { return state_; }

private:
    friend class OperationRegistry;

    OperationGuard(OperationRegistry& registry, std::string key,
                   std::shared_ptr<OperationState> state) noexcept;

    void release() noexcept;

    OperationRegistry* registry_{};
    std::string key_;
    std::shared_ptr<OperationState> state_;
};

class OperationRegistry final {
public:
    [[nodiscard]] Domain::Result<OperationGuard> admit(const Domain::OperationId& operationId);

    void cancel(const Domain::OperationId& operationId) noexcept;
    void cancelAll() noexcept;
    void beginShutdown() noexcept;

    [[nodiscard]] bool waitUntilEmpty(std::chrono::milliseconds timeout) noexcept;

private:
    friend class OperationGuard;

    void release(const std::string& key, const std::shared_ptr<OperationState>& state) noexcept;

    [[nodiscard]] std::vector<std::shared_ptr<OperationState>> snapshotActive() const;

    mutable std::mutex mutex_;
    std::condition_variable emptyCondition_;
    std::unordered_map<std::string, std::shared_ptr<OperationState>> active_;
    bool shuttingDown_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
