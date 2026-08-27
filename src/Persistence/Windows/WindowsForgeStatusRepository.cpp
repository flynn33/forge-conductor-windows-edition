#include "ForgeConductor/Persistence/Windows/WindowsForgeStatusRepository.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Persistence::Windows {
namespace {

using Detail::WinsqliteConnection;
using Detail::WinsqliteStatement;
using Detail::WinsqliteStepResult;

constexpr std::size_t MaximumPersistedIdentifierBytes = 256U;
constexpr std::string_view StatusProjectionSql =
    "WITH open_sessions AS ("
    "SELECT id,created_at FROM agent_sessions "
    "WHERE status IN ('open','active','running','started') "
    "ORDER BY julianday(created_at) DESC,id DESC LIMIT ?"
    "),presence_total AS ("
    "SELECT COUNT(*) AS value FROM client_presence"
    ") "
    "SELECT presence_total.value,open_sessions.id "
    "FROM presence_total LEFT JOIN open_sessions ON 1=1 "
    "ORDER BY julianday(open_sessions.created_at) DESC,open_sessions.id DESC";

static_assert(
    Domain::ForgeStatusLimits::MaximumOpenSessionIds <
    static_cast<std::size_t>(INT64_MAX));

struct RepositoryFailure final {
    Domain::Error error;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw RepositoryFailure{std::move(error)};
}

[[noreturn]] void invalid(std::string message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, std::move(message)));
}

[[noreturn]] void integrity(std::string message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message)));
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        fail(std::move(result).error());
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        fail(std::move(result).error());
    }
}

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        return Domain::Result<T>::success(std::forward<Callable>(callable)());
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Forge status repository operation failed safely."));
    }
}

template <typename Callable>
[[nodiscard]] Domain::Result<void> guardedVoid(Callable&& callable) noexcept
{
    try {
        std::forward<Callable>(callable)();
        return Domain::Result<void>::success();
    } catch (RepositoryFailure& failure) {
        return Domain::Result<void>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Forge status database callback failed safely."));
    }
}

template <typename Callable>
class DatabaseOperation final : public Detail::IWindowsDatabaseOperation {
public:
    explicit DatabaseOperation(Callable callable)
        : callable_{std::move(callable)}
    {
    }

private:
    [[nodiscard]] Domain::Result<void> execute(
        WinsqliteConnection& connection) noexcept override
    {
        try {
            return callable_(connection);
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Forge status database callback failed safely."));
        }
    }

    Callable callable_;
};

template <typename Callable>
[[nodiscard]] Domain::Result<void> runOnStore(
    Detail::WindowsDatabaseStore& store,
    const std::string_view action,
    const Domain::OperationContext& context,
    Callable&& callable) noexcept
{
    using Operation = DatabaseOperation<std::decay_t<Callable>>;
    Operation operation{std::forward<Callable>(callable)};
    return store.runExclusive(operation, action, context);
}

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* const store)
{
    if (store == nullptr) {
        invalid("The Forge status repository is closed.");
    }
    return *store;
}

[[nodiscard]] Domain::SessionId readSessionId(
    const WinsqliteStatement& statement)
{
    auto encoded = statement.columnText(1, MaximumPersistedIdentifierBytes);
    if (!encoded || !encoded.value() || encoded.value()->empty()) {
        integrity("A persisted open-session ID is null, oversized, or empty.");
    }
    auto parsed = Domain::SessionId::parse(encoded.value().value());
    if (!parsed) {
        integrity("A persisted open-session ID is not a canonical UUID.");
    }
    return std::move(parsed).value();
}

} // namespace

struct WindowsForgeStatusRepository::Impl final {
    explicit Impl(std::shared_ptr<WindowsCentralDatabase> ownedDatabase) noexcept
        : database{std::move(ownedDatabase)}
    {
    }

    [[nodiscard]] Detail::WindowsDatabaseStore* repositoryStore() noexcept
    {
        return !closed.load(std::memory_order_acquire) && database
            ? database->repositoryStore()
            : nullptr;
    }

    std::shared_ptr<WindowsCentralDatabase> database;
    std::atomic_bool closed{};
};

WindowsForgeStatusRepository::WindowsForgeStatusRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsForgeStatusRepository::~WindowsForgeStatusRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsForgeStatusRepository>>
WindowsForgeStatusRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database) noexcept
{
    try {
        if (!database) {
            return Domain::Result<
                std::shared_ptr<WindowsForgeStatusRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Forge status repository requires a shared central database."));
        }
        auto implementation = std::make_unique<Impl>(std::move(database));
        return Domain::Result<
            std::shared_ptr<WindowsForgeStatusRepository>>::success(
            std::shared_ptr<WindowsForgeStatusRepository>{
                new WindowsForgeStatusRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsForgeStatusRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Forge status repository could not be constructed."));
    }
}

Domain::Result<Domain::ForgeStatusProjection>
WindowsForgeStatusRepository::snapshot(
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ForgeStatusProjection>([&]() {
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        Domain::ForgeStatusProjection projection;
        projection.openSessionIds.reserve(64U);
        std::optional<std::size_t> observedPresenceCount;

        take(runOnStore(
            store,
            "Read Forge status projection",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    auto statement = take(connection.prepare(
                        StatusProjectionSql, context));
                    take(statement.bindInt64(
                        1,
                        static_cast<std::int64_t>(
                            Domain::ForgeStatusLimits::MaximumOpenSessionIds + 1U)));
                    for (;;) {
                        const auto stepped = take(statement.step());
                        if (stepped == WinsqliteStepResult::Done) {
                            break;
                        }

                        const auto rawPresenceCount =
                            take(statement.columnInt64(0));
                        if (rawPresenceCount < 0 ||
                            static_cast<std::uint64_t>(rawPresenceCount) >
                                static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::size_t>::max)())) {
                            integrity(
                                "The persisted client-presence count is invalid.");
                        }
                        const auto presenceCount =
                            static_cast<std::size_t>(rawPresenceCount);
                        if (observedPresenceCount &&
                            *observedPresenceCount != presenceCount) {
                            integrity(
                                "The Forge status query returned inconsistent presence counts.");
                        }
                        observedPresenceCount = presenceCount;

                        if (take(statement.columnIsNull(1))) {
                            continue;
                        }
                        if (projection.openSessionIds.size() ==
                            Domain::ForgeStatusLimits::MaximumOpenSessionIds) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::LimitExceeded,
                                "Open agent sessions exceed the 10000-session status bound."));
                        }
                        projection.openSessionIds.push_back(
                            readSessionId(statement));
                    }
                });
            }));

        if (!observedPresenceCount) {
            integrity("The Forge status query returned no projection row.");
        }
        projection.presenceCount = *observedPresenceCount;
        take(Domain::validateForgeStatusProjection(projection));
        return projection;
    });
}

void WindowsForgeStatusRepository::close() noexcept
{
    try {
        if (implementation_) {
            implementation_->closed.store(true, std::memory_order_release);
        }
    } catch (...) {
    }
}

} // namespace ForgeConductor::Persistence::Windows
