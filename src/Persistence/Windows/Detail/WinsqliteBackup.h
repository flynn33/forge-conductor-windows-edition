#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "WinsqliteOperationGuard.h"

#include <cstdint>
#include <utility>

struct sqlite3_backup;

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteConnection;

struct WinsqliteBackupProgress final {
    bool complete{};
    std::int32_t remainingPages{};
    std::int32_t totalPages{};
};

class WinsqliteBackup final {
public:
    [[nodiscard]] static Domain::Result<WinsqliteBackup> begin(
        WinsqliteConnection& source,
        WinsqliteConnection& destination,
        const Domain::OperationContext& context) noexcept;

    ~WinsqliteBackup() noexcept;

    WinsqliteBackup(const WinsqliteBackup&) = delete;
    WinsqliteBackup& operator=(const WinsqliteBackup&) = delete;

    WinsqliteBackup(WinsqliteBackup&& other) noexcept;
    WinsqliteBackup& operator=(WinsqliteBackup&& other) noexcept;

    [[nodiscard]] Domain::Result<WinsqliteBackupProgress> step(
        std::int32_t maximumPages) noexcept;
    [[nodiscard]] Domain::Result<WinsqliteBackupProgress> runToCompletion(
        std::int32_t maximumPagesPerStep = 128) noexcept;
    [[nodiscard]] Domain::Result<void> finish() noexcept;

    [[nodiscard]] bool isComplete() const noexcept { return complete_; }
    [[nodiscard]] std::int32_t remainingPages() const noexcept { return remainingPages_; }
    [[nodiscard]] std::int32_t totalPages() const noexcept { return totalPages_; }

private:
    [[nodiscard]] static Domain::Result<WinsqliteBackup> initialize(
        WinsqliteOperationGuard sourceGuard,
        WinsqliteOperationGuard destinationGuard) noexcept;

    WinsqliteBackup(
        sqlite3_backup* backup,
        WinsqliteOperationGuard sourceGuard,
        WinsqliteOperationGuard destinationGuard) noexcept
        : backup_{backup}, sourceGuard_{std::move(sourceGuard)},
          destinationGuard_{std::move(destinationGuard)}
    {
    }

    [[nodiscard]] Domain::Result<void> finishNative() noexcept;
    void finishNoexcept() noexcept;

    sqlite3_backup* backup_{};
    WinsqliteOperationGuard sourceGuard_;
    WinsqliteOperationGuard destinationGuard_;
    std::int32_t remainingPages_{};
    std::int32_t totalPages_{};
    bool complete_{};
};

} // namespace ForgeConductor::Persistence::Windows::Detail
