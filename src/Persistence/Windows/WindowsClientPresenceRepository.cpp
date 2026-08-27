#include "ForgeConductor/Persistence/Windows/WindowsClientPresenceRepository.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
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
            "The client presence repository operation failed safely."));
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
            "The client presence repository operation failed safely."));
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
                "The client presence database callback failed safely."));
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
        invalid("The client presence repository is closed.");
    }
    return *store;
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        integrity("A client presence mutation unexpectedly returned a row.");
    }
}

[[nodiscard]] bool oneRowChanged(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare("SELECT changes()", context));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        integrity("The client presence change count returned no row.");
    }
    const auto changed = take(statement.columnInt64(0));
    if (changed < 0 || changed > 1) {
        integrity("The client presence mutation changed an invalid row count.");
    }
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        integrity("The client presence change count returned extra rows.");
    }
    return changed == 1;
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto elapsed = timestamp.time_since_epoch();
    if (elapsed < Domain::UtcTimePoint::duration::zero()) {
        invalid("Client presence timestamps must not precede the Unix epoch.");
    }
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fractional = milliseconds -
        std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0 || utc.tm_year + 1900 > 9999) {
        invalid("The client presence timestamp is outside the supported UTC range.");
    }
    std::array<char, 25U> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<long long>(fractional.count()));
    if (written != 24) {
        invalid("The client presence timestamp could not be formatted canonically.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

void bindIdentity(
    WinsqliteStatement& statement,
    const int clientParameter,
    const Domain::ClientPresenceIdentity& identity)
{
    take(statement.bindText(clientParameter, identity.clientId.value()));
    take(statement.bindText(clientParameter + 1, identity.role));
    if (identity.deploymentId) {
        take(statement.bindText(
            clientParameter + 2, identity.deploymentId->value()));
    } else {
        take(statement.bindNull(clientParameter + 2));
    }
    if (identity.processId) {
        take(statement.bindInt64(
            clientParameter + 3,
            static_cast<std::int64_t>(*identity.processId)));
    } else {
        take(statement.bindNull(clientParameter + 3));
    }
}

} // namespace

struct WindowsClientPresenceRepository::Impl final {
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

WindowsClientPresenceRepository::WindowsClientPresenceRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsClientPresenceRepository::~WindowsClientPresenceRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsClientPresenceRepository>>
WindowsClientPresenceRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database) noexcept
{
    try {
        if (!database) {
            return Domain::Result<
                std::shared_ptr<WindowsClientPresenceRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The client presence repository requires a shared central database."));
        }
        auto implementation = std::make_unique<Impl>(std::move(database));
        return Domain::Result<
            std::shared_ptr<WindowsClientPresenceRepository>>::success(
            std::shared_ptr<WindowsClientPresenceRepository>{
                new WindowsClientPresenceRepository{
                    std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsClientPresenceRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The client presence repository could not be constructed."));
    }
}

Domain::Result<void> WindowsClientPresenceRepository::upsert(
    const Domain::ClientPresenceRegistration& registration,
    const Domain::OperationContext& context) noexcept
{
    return guardedVoid([&]() {
        take(Domain::validateClientPresenceRegistration(registration));
        const auto firstSeenAt = timestampText(registration.firstSeenAt);
        const auto lastSeenAt = timestampText(registration.lastSeenAt);
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        take(runOnStore(
            store,
            "Upsert client presence",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    {
                        auto statement = take(connection.prepare(
                            "INSERT INTO client_presence("
                            "client_id,role,deployment_id,process_id,first_seen_at,last_seen_at) "
                            "VALUES(?,?,?,?,?,?) ON CONFLICT(client_id) DO UPDATE SET "
                            "role=excluded.role,deployment_id=excluded.deployment_id,"
                            "process_id=excluded.process_id,last_seen_at=excluded.last_seen_at",
                            context));
                        bindIdentity(statement, 1, registration.identity);
                        take(statement.bindText(5, firstSeenAt));
                        take(statement.bindText(6, lastSeenAt));
                        stepDone(statement);
                    }
                    if (!oneRowChanged(connection, context)) {
                        integrity("The client presence upsert did not change one row.");
                    }
                });
            }));
    });
}

Domain::Result<bool> WindowsClientPresenceRepository::heartbeat(
    const Domain::ClientPresenceIdentity& identity,
    const Domain::UtcTimePoint observedAt,
    const Domain::OperationContext& context) noexcept
{
    return guarded<bool>([&]() {
        take(Domain::validateClientPresenceIdentity(identity));
        const auto lastSeenAt = timestampText(observedAt);
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        bool matched{};
        take(runOnStore(
            store,
            "Heartbeat client presence",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    {
                        auto statement = take(connection.prepare(
                            "UPDATE client_presence SET last_seen_at="
                            "CASE WHEN last_seen_at < ? THEN ? ELSE last_seen_at END "
                            "WHERE client_id=? AND role=? AND deployment_id IS ? "
                            "AND process_id IS ?",
                            context));
                        take(statement.bindText(1, lastSeenAt));
                        take(statement.bindText(2, lastSeenAt));
                        bindIdentity(statement, 3, identity);
                        stepDone(statement);
                    }
                    matched = oneRowChanged(connection, context);
                });
            }));
        return matched;
    });
}

Domain::Result<bool> WindowsClientPresenceRepository::remove(
    const Domain::ClientPresenceIdentity& identity,
    const Domain::OperationContext& context) noexcept
{
    return guarded<bool>([&]() {
        take(Domain::validateClientPresenceIdentity(identity));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        bool removed{};
        take(runOnStore(
            store,
            "Remove client presence",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    {
                        auto statement = take(connection.prepare(
                            "DELETE FROM client_presence WHERE client_id=? AND role=? "
                            "AND deployment_id IS ? AND process_id IS ?",
                            context));
                        bindIdentity(statement, 1, identity);
                        stepDone(statement);
                    }
                    removed = oneRowChanged(connection, context);
                });
            }));
        return removed;
    });
}

void WindowsClientPresenceRepository::close() noexcept
{
    try {
        if (implementation_) {
            implementation_->closed.store(true, std::memory_order_release);
        }
    } catch (...) {
    }
}

} // namespace ForgeConductor::Persistence::Windows
