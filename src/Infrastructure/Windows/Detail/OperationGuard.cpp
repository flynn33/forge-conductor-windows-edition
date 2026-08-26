#include "OperationGuard.h"

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/ProcessModels.h"
#include "JobObject.h"
#include "OverlappedPipeReader.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] UINT terminationExitCode(const TerminationReason reason) noexcept
{
    switch (reason) {
    case TerminationReason::ProcessTimeout:
    case TerminationReason::ContextDeadline:
        return ERROR_TIMEOUT;
    case TerminationReason::Cancelled:
    case TerminationReason::Shutdown:
        return ERROR_CANCELLED;
    case TerminationReason::None:
        return ERROR_SUCCESS;
    }
    return ERROR_CANCELLED;
}

} // namespace

Domain::Result<std::shared_ptr<OperationState>> OperationState::create()
{
    UniqueHandle cancellationEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!cancellationEvent) {
        return Domain::Result<std::shared_ptr<OperationState>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "CreateEventW failed for process cancellation with Win32 error " +
                                  std::to_string(::GetLastError()) + "."));
    }
    return Domain::Result<std::shared_ptr<OperationState>>::success(
        std::shared_ptr<OperationState>{new OperationState{std::move(cancellationEvent)}});
}

void OperationState::setJob(std::shared_ptr<JobObject> job) noexcept
{
    TerminationReason currentReason{};
    {
        std::scoped_lock lock{mutex_};
        job_ = job;
        currentReason = reason();
    }
    if (currentReason != TerminationReason::None) {
        static_cast<void>(job->terminate(terminationExitCode(currentReason)));
    }
}

void OperationState::setRuntimeOwnership(Contracts::RuntimeOwnershipLease childProcess,
                                         Contracts::RuntimeOwnershipLease stdoutReader,
                                         Contracts::RuntimeOwnershipLease stderrReader) noexcept
{
    std::scoped_lock lock{mutex_};
    childProcessOwnership_.emplace(std::move(childProcess));
    stdoutReaderOwnership_.emplace(std::move(stdoutReader));
    stderrReaderOwnership_.emplace(std::move(stderrReader));
}

void OperationState::setReaders(std::shared_ptr<OverlappedPipeReader> stdoutReader,
                                std::shared_ptr<OverlappedPipeReader> stderrReader) noexcept
{
    TerminationReason currentReason{};
    {
        std::scoped_lock lock{mutex_};
        stdoutReader_ = stdoutReader;
        stderrReader_ = stderrReader;
        currentReason = reason();
    }
    if (currentReason != TerminationReason::None) {
        stdoutReader->stopImmediately();
        stderrReader->stopImmediately();
    }
}

void OperationState::requestTermination(const TerminationReason requestedReason) noexcept
{
    std::shared_ptr<JobObject> job;
    std::shared_ptr<OverlappedPipeReader> stdoutReader;
    std::shared_ptr<OverlappedPipeReader> stderrReader;
    {
        std::scoped_lock lock{mutex_};
        auto expected = TerminationReason::None;
        static_cast<void>(reason_.compare_exchange_strong(
            expected, requestedReason, std::memory_order_acq_rel, std::memory_order_acquire));
        job = job_;
        stdoutReader = stdoutReader_;
        stderrReader = stderrReader_;
    }
    static_cast<void>(::SetEvent(cancellationEvent_.get()));

    const auto effectiveReason = reason();
    if (job) {
        static_cast<void>(job->terminate(terminationExitCode(effectiveReason)));
    }
    if (stdoutReader) {
        stdoutReader->stopImmediately();
    }
    if (stderrReader) {
        stderrReader->stopImmediately();
    }
}

Domain::Result<bool> OperationState::resumePrimaryThread(const HANDLE primaryThread) noexcept
{
    std::scoped_lock lock{mutex_};
    if (reason() != TerminationReason::None) {
        return Domain::Result<bool>::success(false);
    }
    if (::ResumeThread(primaryThread) == (std::numeric_limits<DWORD>::max)()) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "ResumeThread failed with Win32 error " + std::to_string(::GetLastError()) + "."));
    }
    return Domain::Result<bool>::success(true);
}

void OperationState::finishReadersAfterDirectChildExit() noexcept
{
    std::shared_ptr<OverlappedPipeReader> stdoutReader;
    std::shared_ptr<OverlappedPipeReader> stderrReader;
    {
        std::scoped_lock lock{mutex_};
        stdoutReader = stdoutReader_;
        stderrReader = stderrReader_;
    }
    if (stdoutReader) {
        stdoutReader->finishAvailable();
    }
    if (stderrReader) {
        stderrReader->finishAvailable();
    }
}

void OperationState::stopReadersImmediately() noexcept
{
    std::shared_ptr<OverlappedPipeReader> stdoutReader;
    std::shared_ptr<OverlappedPipeReader> stderrReader;
    {
        std::scoped_lock lock{mutex_};
        stdoutReader = stdoutReader_;
        stderrReader = stderrReader_;
    }
    if (stdoutReader) {
        stdoutReader->stopImmediately();
    }
    if (stderrReader) {
        stderrReader->stopImmediately();
    }
}

void OperationState::releaseResources() noexcept
{
    std::shared_ptr<JobObject> job;
    std::shared_ptr<OverlappedPipeReader> stdoutReader;
    std::shared_ptr<OverlappedPipeReader> stderrReader;
    std::optional<Contracts::RuntimeOwnershipLease> childProcessOwnership;
    std::optional<Contracts::RuntimeOwnershipLease> stdoutReaderOwnership;
    std::optional<Contracts::RuntimeOwnershipLease> stderrReaderOwnership;
    {
        std::scoped_lock lock{mutex_};
        job = std::move(job_);
        stdoutReader = std::move(stdoutReader_);
        stderrReader = std::move(stderrReader_);
        childProcessOwnership = std::move(childProcessOwnership_);
        stdoutReaderOwnership = std::move(stdoutReaderOwnership_);
        stderrReaderOwnership = std::move(stderrReaderOwnership_);
    }
    if (stdoutReader) {
        stdoutReader->cancelAndWait();
        stdoutReader.reset();
    }
    if (stderrReader) {
        stderrReader->cancelAndWait();
        stderrReader.reset();
    }
    job.reset();
    stderrReaderOwnership.reset();
    stdoutReaderOwnership.reset();
    childProcessOwnership.reset();
}

OperationGuard::OperationGuard(OperationRegistry& registry, std::string key,
                               std::shared_ptr<OperationState> state) noexcept
    : registry_{&registry}, key_{std::move(key)}, state_{std::move(state)}
{
}

OperationGuard::~OperationGuard() noexcept { release(); }

OperationGuard::OperationGuard(OperationGuard&& other) noexcept
    : registry_{std::exchange(other.registry_, nullptr)}, key_{std::move(other.key_)},
      state_{std::move(other.state_)}
{
}

OperationGuard& OperationGuard::operator=(OperationGuard&& other) noexcept
{
    if (this != &other) {
        release();
        registry_ = std::exchange(other.registry_, nullptr);
        key_ = std::move(other.key_);
        state_ = std::move(other.state_);
    }
    return *this;
}

void OperationGuard::release() noexcept
{
    if (registry_ != nullptr) {
        state_->releaseResources();
        registry_->release(key_, state_);
        registry_ = nullptr;
    }
}

Domain::Result<OperationGuard> OperationRegistry::admit(const Domain::OperationId& operationId)
{
    auto state = OperationState::create();
    if (!state) {
        return Domain::Result<OperationGuard>::failure(std::move(state).error());
    }

    const auto key = operationId.value();
    std::scoped_lock lock{mutex_};
    if (shuttingDown_) {
        return Domain::Result<OperationGuard>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled, "The process supervisor is shutting down."));
    }
    if (active_.contains(key)) {
        return Domain::Result<OperationGuard>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict, "The process operation identifier is already active."));
    }
    if (active_.size() >= Domain::MaximumConcurrentProcessOperations) {
        return Domain::Result<OperationGuard>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded, "The process supervisor already owns 64 active "
                                               "operations; no work was queued."));
    }

    auto sharedState = std::move(state).value();
    active_.emplace(key, sharedState);
    return Domain::Result<OperationGuard>::success(
        OperationGuard{*this, key, std::move(sharedState)});
}

void OperationRegistry::cancel(const Domain::OperationId& operationId) noexcept
{
    std::shared_ptr<OperationState> state;
    {
        std::scoped_lock lock{mutex_};
        const auto match = active_.find(operationId.value());
        if (match != active_.end()) {
            state = match->second;
        }
    }
    if (state) {
        state->requestTermination(TerminationReason::Cancelled);
    }
}

std::vector<std::shared_ptr<OperationState>> OperationRegistry::snapshotActive() const
{
    std::vector<std::shared_ptr<OperationState>> states;
    std::scoped_lock lock{mutex_};
    states.reserve(active_.size());
    std::transform(active_.begin(), active_.end(), std::back_inserter(states),
                   [](const auto& entry) { return entry.second; });
    return states;
}

void OperationRegistry::cancelAll() noexcept
{
    try {
        for (const auto& state : snapshotActive()) {
            state->requestTermination(TerminationReason::Cancelled);
        }
    } catch (...) {
    }
}

void OperationRegistry::beginShutdown() noexcept
{
    try {
        std::vector<std::shared_ptr<OperationState>> states;
        {
            std::scoped_lock lock{mutex_};
            shuttingDown_ = true;
            states.reserve(active_.size());
            std::transform(active_.begin(), active_.end(), std::back_inserter(states),
                           [](const auto& entry) { return entry.second; });
        }
        for (const auto& state : states) {
            state->requestTermination(TerminationReason::Shutdown);
        }
    } catch (...) {
    }
}

bool OperationRegistry::waitUntilEmpty(const std::chrono::milliseconds timeout) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        return emptyCondition_.wait_for(lock, timeout, [this] { return active_.empty(); });
    } catch (...) {
        return false;
    }
}

void OperationRegistry::release(const std::string& key,
                                const std::shared_ptr<OperationState>& state) noexcept
{
    try {
        {
            std::scoped_lock lock{mutex_};
            const auto match = active_.find(key);
            if (match != active_.end() && match->second == state) {
                active_.erase(match);
            }
        }
        emptyCondition_.notify_all();
    } catch (...) {
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
