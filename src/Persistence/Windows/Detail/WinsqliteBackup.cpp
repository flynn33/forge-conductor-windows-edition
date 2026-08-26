#include "WinsqliteBackup.h"

#include "WinsqliteConnection.h"

#include <winsqlite/winsqlite3.h>

#include <functional>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

inline constexpr std::int32_t MaximumBackupPageChunk = 1'024;

} // namespace

Domain::Result<WinsqliteBackup> WinsqliteBackup::initialize(
    WinsqliteOperationGuard sourceGuard,
    WinsqliteOperationGuard destinationGuard) noexcept
{
    try {
        sqlite3* const sourceDatabase = WinsqliteOperationGuard::database(
            sourceGuard.shareState());
        sqlite3* const destinationDatabase = WinsqliteOperationGuard::database(
            destinationGuard.shareState());
        if (sourceDatabase == nullptr || destinationDatabase == nullptr) {
            return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The online backup requires two open Winsqlite3 connections."));
        }

        sqlite3_backup* const backup = sqlite3_backup_init(
            destinationDatabase, "main", sourceDatabase, "main");
        if (backup == nullptr) {
            const int result = sqlite3_extended_errcode(destinationDatabase);
            return Domain::Result<WinsqliteBackup>::failure(
                WinsqliteOperationGuard::error(
                    destinationGuard.shareState(), result,
                    "initialize the online database backup"));
        }
        return Domain::Result<WinsqliteBackup>::success(WinsqliteBackup{
            backup, std::move(sourceGuard), std::move(destinationGuard)});
    } catch (...) {
        return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 online backup could not be initialized."));
    }
}

Domain::Result<WinsqliteBackup> WinsqliteBackup::begin(
    WinsqliteConnection& source,
    WinsqliteConnection& destination,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (source.state_ == nullptr || destination.state_ == nullptr) {
            return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The online backup requires two owned Winsqlite3 connections."));
        }
        if (source.state_.get() == destination.state_.get()) {
            return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The online backup source and destination must be different connections."));
        }
        if (destination.state_->readOnly_) {
            return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The online backup destination must be a writable connection."));
        }

        const bool sourceFirst = std::less<const void*>{}(
            source.state_.get(), destination.state_.get());
        if (sourceFirst) {
            auto sourceResult = WinsqliteOperationGuard::acquire(source, context);
            if (!sourceResult) {
                return Domain::Result<WinsqliteBackup>::failure(
                    std::move(sourceResult).error());
            }
            auto destinationResult = WinsqliteOperationGuard::acquire(destination, context);
            if (!destinationResult) {
                return Domain::Result<WinsqliteBackup>::failure(
                    std::move(destinationResult).error());
            }
            return initialize(
                std::move(sourceResult).value(), std::move(destinationResult).value());
        }

        auto destinationResult = WinsqliteOperationGuard::acquire(destination, context);
        if (!destinationResult) {
            return Domain::Result<WinsqliteBackup>::failure(
                std::move(destinationResult).error());
        }
        auto sourceResult = WinsqliteOperationGuard::acquire(source, context);
        if (!sourceResult) {
            return Domain::Result<WinsqliteBackup>::failure(
                std::move(sourceResult).error());
        }
        return initialize(
            std::move(sourceResult).value(), std::move(destinationResult).value());
    } catch (...) {
        return Domain::Result<WinsqliteBackup>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 online backup operation could not be created."));
    }
}

WinsqliteBackup::~WinsqliteBackup() noexcept
{
    finishNoexcept();
}

WinsqliteBackup::WinsqliteBackup(WinsqliteBackup&& other) noexcept
    : backup_{std::exchange(other.backup_, nullptr)},
      sourceGuard_{std::move(other.sourceGuard_)},
      destinationGuard_{std::move(other.destinationGuard_)},
      remainingPages_{std::exchange(other.remainingPages_, 0)},
      totalPages_{std::exchange(other.totalPages_, 0)},
      complete_{std::exchange(other.complete_, false)}
{
}

WinsqliteBackup& WinsqliteBackup::operator=(WinsqliteBackup&& other) noexcept
{
    if (this != &other) {
        finishNoexcept();
        backup_ = std::exchange(other.backup_, nullptr);
        sourceGuard_ = std::move(other.sourceGuard_);
        destinationGuard_ = std::move(other.destinationGuard_);
        remainingPages_ = std::exchange(other.remainingPages_, 0);
        totalPages_ = std::exchange(other.totalPages_, 0);
        complete_ = std::exchange(other.complete_, false);
    }
    return *this;
}

Domain::Result<WinsqliteBackupProgress> WinsqliteBackup::step(
    const std::int32_t maximumPages) noexcept
{
    try {
        if (backup_ == nullptr) {
            if (complete_) {
                return Domain::Result<WinsqliteBackupProgress>::success(
                    WinsqliteBackupProgress{true, remainingPages_, totalPages_});
            }
            return Domain::Result<WinsqliteBackupProgress>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Winsqlite3 online backup is already finalized."));
        }
        if (maximumPages < 1 || maximumPages > MaximumBackupPageChunk) {
            return Domain::Result<WinsqliteBackupProgress>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 backup step must copy between 1 and 1024 pages."));
        }

        int retryCount = 0;
        for (;;) {
            auto contextCheck = sourceGuard_.check("copy an online database backup chunk");
            if (!contextCheck) {
                return Domain::Result<WinsqliteBackupProgress>::failure(
                    std::move(contextCheck).error());
            }
            contextCheck = destinationGuard_.check(
                "copy an online database backup chunk");
            if (!contextCheck) {
                return Domain::Result<WinsqliteBackupProgress>::failure(
                    std::move(contextCheck).error());
            }

            const int result = sqlite3_backup_step(backup_, maximumPages);
            remainingPages_ = sqlite3_backup_remaining(backup_);
            totalPages_ = sqlite3_backup_pagecount(backup_);
            if (result == SQLITE_OK) {
                return Domain::Result<WinsqliteBackupProgress>::success(
                    WinsqliteBackupProgress{false, remainingPages_, totalPages_});
            }
            if (result == SQLITE_DONE) {
                auto finished = finishNative();
                if (!finished) {
                    return Domain::Result<WinsqliteBackupProgress>::failure(
                        std::move(finished).error());
                }
                complete_ = true;
                remainingPages_ = 0;
                return Domain::Result<WinsqliteBackupProgress>::success(
                    WinsqliteBackupProgress{true, remainingPages_, totalPages_});
            }
            if ((result & 0xFF) == SQLITE_BUSY || (result & 0xFF) == SQLITE_LOCKED) {
                auto retry = WinsqliteOperationGuard::waitForBusyRetry(
                    destinationGuard_.shareState(), retryCount,
                    "wait for the online database backup");
                if (!retry) {
                    return Domain::Result<WinsqliteBackupProgress>::failure(
                        std::move(retry).error());
                }
                ++retryCount;
                continue;
            }
            return Domain::Result<WinsqliteBackupProgress>::failure(
                WinsqliteOperationGuard::error(
                    destinationGuard_.shareState(), result,
                    "copy an online database backup chunk"));
        }
    } catch (...) {
        return Domain::Result<WinsqliteBackupProgress>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 online backup step could not be completed."));
    }
}

Domain::Result<WinsqliteBackupProgress> WinsqliteBackup::runToCompletion(
    const std::int32_t maximumPagesPerStep) noexcept
{
    try {
        for (;;) {
            auto progress = step(maximumPagesPerStep);
            if (!progress) {
                return progress;
            }
            if (progress.value().complete) {
                return progress;
            }
        }
    } catch (...) {
        return Domain::Result<WinsqliteBackupProgress>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 online backup could not run to completion."));
    }
}

Domain::Result<void> WinsqliteBackup::finishNative() noexcept
{
    try {
        if (backup_ == nullptr) {
            return Domain::Result<void>::success();
        }
        sqlite3_backup* const backup = std::exchange(backup_, nullptr);
        const int result = sqlite3_backup_finish(backup);
        if (result != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                destinationGuard_.shareState(), result,
                "finalize the online database backup"));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 online backup could not be finalized."));
    }
}

Domain::Result<void> WinsqliteBackup::finish() noexcept
{
    return finishNative();
}

void WinsqliteBackup::finishNoexcept() noexcept
{
    if (backup_ != nullptr) {
        sqlite3_backup* const backup = std::exchange(backup_, nullptr);
        static_cast<void>(sqlite3_backup_finish(backup));
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
