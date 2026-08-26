#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "WinsqliteError.h"

#include <memory>
#include <string_view>
#include <utility>

struct sqlite3;

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteBackup;
class WinsqliteConnection;
class WinsqliteStatement;
class WinsqliteTransaction;
struct WinsqliteOperationState;

class WinsqliteOperationGuard final {
public:
    [[nodiscard]] static Domain::Result<WinsqliteOperationGuard> acquire(
        WinsqliteConnection& connection,
        const Domain::OperationContext& context) noexcept;

    ~WinsqliteOperationGuard() noexcept;

    WinsqliteOperationGuard(const WinsqliteOperationGuard&) = delete;
    WinsqliteOperationGuard& operator=(const WinsqliteOperationGuard&) = delete;

    WinsqliteOperationGuard(WinsqliteOperationGuard&& other) noexcept;
    WinsqliteOperationGuard& operator=(WinsqliteOperationGuard&& other) noexcept;

    [[nodiscard]] Domain::Result<void> check(std::string_view action) const noexcept;

private:
    explicit WinsqliteOperationGuard(
        std::shared_ptr<WinsqliteOperationState> state) noexcept
        : state_{std::move(state)}
    {
    }

    [[nodiscard]] static Domain::Result<WinsqliteOperationGuard> acquireForClose(
        WinsqliteConnection& connection,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<WinsqliteOperationGuard> acquireImpl(
        WinsqliteConnection& connection,
        const Domain::OperationContext& context,
        bool allowPoisoned,
        bool allowClosed) noexcept;

    [[nodiscard]] std::shared_ptr<WinsqliteOperationState> shareState() const noexcept;
    [[nodiscard]] std::shared_ptr<WinsqliteOperationState> releaseState() noexcept;

    [[nodiscard]] static sqlite3* database(
        const std::shared_ptr<WinsqliteOperationState>& state) noexcept;

    [[nodiscard]] static Domain::Result<void> check(
        const std::shared_ptr<WinsqliteOperationState>& state,
        std::string_view action) noexcept;

    [[nodiscard]] static Domain::Error error(
        const std::shared_ptr<WinsqliteOperationState>& state,
        int nativeCode,
        std::string_view action) noexcept;

    [[nodiscard]] static Domain::Result<bool> waitForBusyRetry(
        const std::shared_ptr<WinsqliteOperationState>& state,
        int priorRetries,
        std::string_view action) noexcept;

    [[nodiscard]] static int executeCleanupSql(
        const std::shared_ptr<WinsqliteOperationState>& state,
        std::string_view sql) noexcept;

    static void markClosed(
        const std::shared_ptr<WinsqliteOperationState>& state) noexcept;
    static void markPoisoned(
        const std::shared_ptr<WinsqliteOperationState>& state) noexcept;

    friend class WinsqliteBackup;
    friend class WinsqliteConnection;
    friend class WinsqliteStatement;
    friend class WinsqliteTransaction;

    std::shared_ptr<WinsqliteOperationState> state_;
};

} // namespace ForgeConductor::Persistence::Windows::Detail
