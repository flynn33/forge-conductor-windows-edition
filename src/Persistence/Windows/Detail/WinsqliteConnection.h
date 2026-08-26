#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "WinsqliteStatement.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3;

namespace ForgeConductor::Persistence::Windows::Migrations {
class SchemaMigrator;
}

namespace ForgeConductor::Persistence::Windows {
class DatabaseBackupCoordinator;
}

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteBackup;
class DatabaseNamespaceLease;
class WinsqliteOperationGuard;
struct WinsqliteOperationState;
class WinsqliteTransaction;

enum class WinsqliteSynchronousMode { Full, Normal };
enum class WinsqliteOpenMode { ReadOnlyExisting, ReadWriteExisting, ReadWriteCreate };
enum class WinsqliteJournalMode { WriteAheadLog, Delete };

struct WinsqliteConnectionOptions final {
    std::string vfsName;
    WinsqliteOpenMode openMode{WinsqliteOpenMode::ReadOnlyExisting};
    WinsqliteSynchronousMode synchronousMode{WinsqliteSynchronousMode::Full};
    WinsqliteJournalMode journalMode{WinsqliteJournalMode::WriteAheadLog};
    std::shared_ptr<DatabaseNamespaceLease> namespaceAuthority;
};

class WinsqliteConnection final {
public:
    [[nodiscard]] static Domain::Result<WinsqliteConnection> open(
        std::wstring_view databasePath,
        const WinsqliteConnectionOptions& options,
        const Domain::OperationContext& context) noexcept;

    ~WinsqliteConnection() noexcept;

    WinsqliteConnection(const WinsqliteConnection&) = delete;
    WinsqliteConnection& operator=(const WinsqliteConnection&) = delete;

    WinsqliteConnection(WinsqliteConnection&& other) noexcept;
    WinsqliteConnection& operator=(WinsqliteConnection&& other) noexcept;

    [[nodiscard]] Domain::Result<WinsqliteStatement> prepare(
        std::string_view sql,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<void> execute(
        std::string_view sql,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<void> close(
        const Domain::OperationContext& context) noexcept;

    // Integrity recovery marks the connection poisoned before native close so
    // fallback release cannot return the connection to ordinary operations.
    [[nodiscard]] Domain::Result<void> closeForQuarantine(
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] bool belongsToNamespace(
        const DatabaseNamespaceLease& namespaceAuthority) const noexcept;

private:
    class State final {
    public:
        State(
            sqlite3* database,
            const bool readOnly,
            std::shared_ptr<DatabaseNamespaceLease> namespaceAuthority) noexcept
            : database_{database}, readOnly_{readOnly},
              namespaceAuthority_{std::move(namespaceAuthority)}
        {
        }
        ~State() noexcept;

        State(const State&) = delete;
        State& operator=(const State&) = delete;
        State(State&&) = delete;
        State& operator=(State&&) = delete;

    private:
        friend class WinsqliteBackup;
        friend class WinsqliteConnection;
        friend class WinsqliteOperationGuard;
        friend class WinsqliteTransaction;
        friend struct WinsqliteOperationState;
        friend class ForgeConductor::Persistence::Windows::DatabaseBackupCoordinator;

        sqlite3* database_{};
        std::timed_mutex mutex_;
        bool poisoned_{};
        bool readOnly_{};
        std::shared_ptr<DatabaseNamespaceLease> namespaceAuthority_;
    };

    explicit WinsqliteConnection(std::shared_ptr<State> state) noexcept
        : state_{std::move(state)}
    {
    }

    friend class WinsqliteBackup;
    friend class WinsqliteOperationGuard;
    friend class WinsqliteTransaction;
    friend struct WinsqliteOperationState;
    friend class ForgeConductor::Persistence::Windows::DatabaseBackupCoordinator;
    friend class ForgeConductor::Persistence::Windows::Migrations::SchemaMigrator;

    [[nodiscard]] static Domain::Result<std::int64_t> readIntegerPragma(
        const std::shared_ptr<WinsqliteOperationState>& operation,
        std::string_view sql) noexcept;
    [[nodiscard]] static Domain::Result<std::string> readTextPragma(
        const std::shared_ptr<WinsqliteOperationState>& operation,
        std::string_view sql) noexcept;
    [[nodiscard]] static Domain::Result<void> configureConnection(
        const std::shared_ptr<WinsqliteOperationState>& operation,
        const WinsqliteConnectionOptions& options,
        const Domain::OperationContext& context) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace ForgeConductor::Persistence::Windows::Detail
