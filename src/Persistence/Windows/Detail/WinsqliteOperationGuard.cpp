#include "WinsqliteOperationGuard.h"

#include "WinsqliteConnection.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

using namespace std::chrono_literals;

inline constexpr auto MaximumBusyWait = 3s;
inline constexpr auto MaximumWaitSlice = 10ms;
inline constexpr int ProgressInstructionInterval = 1'000;
inline constexpr std::size_t MaximumCleanupSqlBytes = 128U;

[[nodiscard]] Domain::Result<void> contextResult(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The database operation was cancelled before Forge Conductor could " +
                    std::string{action} + "."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The database operation deadline expired before Forge Conductor could " +
                    std::string{action} + "."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The database operation context could not be validated."));
    }
}

} // namespace

struct WinsqliteOperationState final {
    WinsqliteOperationState(
        std::shared_ptr<WinsqliteConnection::State> connection,
        std::unique_lock<std::timed_mutex> lock,
        Domain::OperationContext context) noexcept
        : connection_{std::move(connection)}, lock_{std::move(lock)},
          context_{std::move(context)}
    {
    }

    ~WinsqliteOperationState() noexcept
    {
        if (callbacksInstalled_ && connection_ != nullptr &&
            connection_->database_ != nullptr) {
            sqlite3_progress_handler(connection_->database_, 0, nullptr, nullptr);
            static_cast<void>(sqlite3_busy_handler(connection_->database_, nullptr, nullptr));
        }
    }

    WinsqliteOperationState(const WinsqliteOperationState&) = delete;
    WinsqliteOperationState& operator=(const WinsqliteOperationState&) = delete;
    WinsqliteOperationState(WinsqliteOperationState&&) = delete;
    WinsqliteOperationState& operator=(WinsqliteOperationState&&) = delete;

    [[nodiscard]] bool waitForBusy(const int priorRetries) noexcept
    {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (context_.isCancellationRequested()) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::Cancelled, std::memory_order_release);
                return false;
            }
            if (context_.isExpired(now)) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::DeadlineExceeded, std::memory_order_release);
                return false;
            }

            if (priorRetries <= 0 || !busyStarted_.has_value()) {
                busyStarted_ = now;
            }
            const auto busyDeadline = (std::min)(
                context_.deadline, busyStarted_.value() + MaximumBusyWait);
            if (now >= busyDeadline) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::BusyTimeout, std::memory_order_release);
                return false;
            }

            const auto remaining = busyDeadline - now;
            auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                remaining);
            if (waitDuration < 1ms) {
                waitDuration = 1ms;
            }
            std::this_thread::sleep_for((std::min)(waitDuration, MaximumWaitSlice));

            const auto afterWait = std::chrono::steady_clock::now();
            if (context_.isCancellationRequested()) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::Cancelled, std::memory_order_release);
                return false;
            }
            if (context_.isExpired(afterWait)) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::DeadlineExceeded, std::memory_order_release);
                return false;
            }
            if (afterWait >= busyDeadline) {
                interruptionReason_.store(
                    WinsqliteInterruptionReason::BusyTimeout, std::memory_order_release);
                return false;
            }
            return true;
        } catch (...) {
            interruptionReason_.store(
                WinsqliteInterruptionReason::BusyTimeout, std::memory_order_release);
            return false;
        }
    }

    [[nodiscard]] static int busyCallback(void* const opaque, const int priorRetries) noexcept
    {
        if (opaque == nullptr) {
            return 0;
        }
        return static_cast<WinsqliteOperationState*>(opaque)->waitForBusy(priorRetries) ? 1 : 0;
    }

    [[nodiscard]] static int progressCallback(void* const opaque) noexcept
    {
        if (opaque == nullptr) {
            return 1;
        }
        auto& state = *static_cast<WinsqliteOperationState*>(opaque);
        if (state.context_.isCancellationRequested()) {
            state.interruptionReason_.store(
                WinsqliteInterruptionReason::Cancelled, std::memory_order_release);
            return 1;
        }
        if (state.context_.isExpired(std::chrono::steady_clock::now())) {
            state.interruptionReason_.store(
                WinsqliteInterruptionReason::DeadlineExceeded, std::memory_order_release);
            return 1;
        }
        return 0;
    }

    std::shared_ptr<WinsqliteConnection::State> connection_;
    std::unique_lock<std::timed_mutex> lock_;
    Domain::OperationContext context_;
    std::atomic<WinsqliteInterruptionReason> interruptionReason_{
        WinsqliteInterruptionReason::None};
    std::optional<Domain::MonotonicTimePoint> busyStarted_;
    bool callbacksInstalled_{};
};

Domain::Result<WinsqliteOperationGuard> WinsqliteOperationGuard::acquire(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    return acquireImpl(connection, context, false, false);
}

Domain::Result<WinsqliteOperationGuard> WinsqliteOperationGuard::acquireForClose(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    return acquireImpl(connection, context, true, true);
}

Domain::Result<WinsqliteOperationGuard> WinsqliteOperationGuard::acquireImpl(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context,
    const bool allowPoisoned,
    const bool allowClosed) noexcept
{
    try {
        auto validContext = contextResult(context, "acquire the database connection");
        if (!validContext) {
            return Domain::Result<WinsqliteOperationGuard>::failure(
                std::move(validContext).error());
        }

        auto connectionState = connection.state_;
        if (connectionState == nullptr) {
            return Domain::Result<WinsqliteOperationGuard>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The database connection has no owned Winsqlite3 state."));
        }

        std::unique_lock<std::timed_mutex> lock{connectionState->mutex_, std::defer_lock};
        const auto waitStarted = std::chrono::steady_clock::now();
        const auto waitDeadline = (std::min)(context.deadline, waitStarted + MaximumBusyWait);
        for (;;) {
            validContext = contextResult(context, "serialize the database operation");
            if (!validContext) {
                return Domain::Result<WinsqliteOperationGuard>::failure(
                    std::move(validContext).error());
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= waitDeadline) {
                validContext = contextResult(context, "serialize the database operation");
                if (!validContext) {
                    return Domain::Result<WinsqliteOperationGuard>::failure(
                        std::move(validContext).error());
                }
                return Domain::Result<WinsqliteOperationGuard>::failure(Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The database connection remained owned by another operation for 3000 ms.",
                    true));
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                waitDeadline - now);
            if (remaining < 1ms) {
                remaining = 1ms;
            }
            if (lock.try_lock_for((std::min)(remaining, MaximumWaitSlice))) {
                break;
            }
        }

        validContext = contextResult(context, "start the database operation");
        if (!validContext) {
            return Domain::Result<WinsqliteOperationGuard>::failure(
                std::move(validContext).error());
        }
        if (connectionState->database_ == nullptr && !allowClosed) {
            return Domain::Result<WinsqliteOperationGuard>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Winsqlite3 connection is closed."));
        }
        if (connectionState->poisoned_ && !allowPoisoned) {
            return Domain::Result<WinsqliteOperationGuard>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The Winsqlite3 connection was quarantined after transaction cleanup failed."));
        }

        auto operation = std::make_shared<WinsqliteOperationState>(
            connectionState, std::move(lock), context);
        if (connectionState->database_ == nullptr) {
            return Domain::Result<WinsqliteOperationGuard>::success(
                WinsqliteOperationGuard{std::move(operation)});
        }
        const int busyResult = sqlite3_busy_handler(
            connectionState->database_, &WinsqliteOperationState::busyCallback, operation.get());
        if (busyResult != SQLITE_OK) {
            static_cast<void>(
                sqlite3_busy_handler(connectionState->database_, nullptr, nullptr));
            return Domain::Result<WinsqliteOperationGuard>::failure(makeWinsqliteError(
                busyResult,
                "install the bounded busy handler",
                sqlite3_errmsg(connectionState->database_),
                WinsqliteInterruptionReason::None,
                &context));
        }
        sqlite3_progress_handler(
            connectionState->database_, ProgressInstructionInterval,
            &WinsqliteOperationState::progressCallback, operation.get());
        operation->callbacksInstalled_ = true;

        return Domain::Result<WinsqliteOperationGuard>::success(
            WinsqliteOperationGuard{std::move(operation)});
    } catch (...) {
        return Domain::Result<WinsqliteOperationGuard>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 operation guard could not be created."));
    }
}

WinsqliteOperationGuard::~WinsqliteOperationGuard() noexcept = default;

WinsqliteOperationGuard::WinsqliteOperationGuard(WinsqliteOperationGuard&& other) noexcept
    : state_{std::move(other.state_)}
{
}

WinsqliteOperationGuard& WinsqliteOperationGuard::operator=(
    WinsqliteOperationGuard&& other) noexcept
{
    if (this != &other) {
        state_ = std::move(other.state_);
    }
    return *this;
}

Domain::Result<void> WinsqliteOperationGuard::check(
    const std::string_view action) const noexcept
{
    return check(state_, action);
}

std::shared_ptr<WinsqliteOperationState> WinsqliteOperationGuard::shareState() const noexcept
{
    return state_;
}

std::shared_ptr<WinsqliteOperationState> WinsqliteOperationGuard::releaseState() noexcept
{
    return std::move(state_);
}

sqlite3* WinsqliteOperationGuard::database(
    const std::shared_ptr<WinsqliteOperationState>& state) noexcept
{
    if (state == nullptr || state->connection_ == nullptr) {
        return nullptr;
    }
    return state->connection_->database_;
}

Domain::Result<void> WinsqliteOperationGuard::check(
    const std::shared_ptr<WinsqliteOperationState>& state,
    const std::string_view action) noexcept
{
    if (state == nullptr || state->connection_ == nullptr ||
        state->connection_->database_ == nullptr) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 operation no longer owns an open connection."));
    }
    return contextResult(state->context_, action);
}

Domain::Error WinsqliteOperationGuard::error(
    const std::shared_ptr<WinsqliteOperationState>& state,
    const int nativeCode,
    const std::string_view action) noexcept
{
    sqlite3* const databaseHandle = database(state);
    const char* const nativeMessage = databaseHandle != nullptr
        ? sqlite3_errmsg(databaseHandle)
        : sqlite3_errstr(nativeCode);
    const auto interruptionReason = state != nullptr
        ? state->interruptionReason_.load(std::memory_order_acquire)
        : WinsqliteInterruptionReason::None;
    const Domain::OperationContext* const context = state != nullptr ? &state->context_ : nullptr;
    return makeWinsqliteError(
        nativeCode, action, nativeMessage, interruptionReason, context);
}

Domain::Result<bool> WinsqliteOperationGuard::waitForBusyRetry(
    const std::shared_ptr<WinsqliteOperationState>& state,
    const int priorRetries,
    const std::string_view action) noexcept
{
    try {
        if (state == nullptr) {
            return Domain::Result<bool>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Winsqlite3 busy retry has no owned operation state."));
        }
        if (!state->waitForBusy(priorRetries)) {
            return Domain::Result<bool>::failure(error(state, SQLITE_BUSY, action));
        }
        return Domain::Result<bool>::success(true);
    } catch (...) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 busy retry could not be bounded."));
    }
}

int WinsqliteOperationGuard::executeCleanupSql(
    const std::shared_ptr<WinsqliteOperationState>& state,
    const std::string_view sql) noexcept
{
    try {
        sqlite3* const databaseHandle = database(state);
        if (databaseHandle == nullptr || sql.empty() ||
            sql.size() > MaximumCleanupSqlBytes || sql.find('\0') != std::string_view::npos) {
            return SQLITE_MISUSE;
        }

        sqlite3_progress_handler(databaseHandle, 0, nullptr, nullptr);
        static_cast<void>(sqlite3_busy_handler(databaseHandle, nullptr, nullptr));

        const std::string terminatedSql{sql};
        char* errorMessage = nullptr;
        int result = sqlite3_exec(
            databaseHandle, terminatedSql.c_str(), nullptr, nullptr, &errorMessage);
        if (errorMessage != nullptr) {
            sqlite3_free(errorMessage);
        }

        const int busyResult = sqlite3_busy_handler(
            databaseHandle, &WinsqliteOperationState::busyCallback, state.get());
        sqlite3_progress_handler(
            databaseHandle, ProgressInstructionInterval,
            &WinsqliteOperationState::progressCallback, state.get());
        if (result == SQLITE_OK && busyResult != SQLITE_OK) {
            result = busyResult;
        }
        return result;
    } catch (...) {
        return SQLITE_NOMEM;
    }
}

void WinsqliteOperationGuard::markClosed(
    const std::shared_ptr<WinsqliteOperationState>& state) noexcept
{
    if (state != nullptr && state->connection_ != nullptr) {
        state->connection_->database_ = nullptr;
        state->callbacksInstalled_ = false;
    }
}

void WinsqliteOperationGuard::markPoisoned(
    const std::shared_ptr<WinsqliteOperationState>& state) noexcept
{
    if (state != nullptr && state->connection_ != nullptr) {
        state->connection_->poisoned_ = true;
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
