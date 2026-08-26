#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include "Detail/WindowsDatabaseStore.h"

#include <memory>
#include <utility>

namespace ForgeConductor::Persistence::Windows {

struct WindowsCentralDatabase::Impl final {
    explicit Impl(std::unique_ptr<Detail::WindowsDatabaseStore> value) noexcept
        : store{std::move(value)}
    {
    }

    std::unique_ptr<Detail::WindowsDatabaseStore> store;
};

WindowsCentralDatabase::WindowsCentralDatabase(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsCentralDatabase::~WindowsCentralDatabase() noexcept = default;

Domain::Result<std::unique_ptr<WindowsCentralDatabase>> WindowsCentralDatabase::open(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!applicationPaths) {
            return Domain::Result<std::unique_ptr<WindowsCentralDatabase>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The central database requires application-owned path resolution."));
        }
        auto root = applicationPaths->dataRoot(context);
        if (!root) {
            return Domain::Result<std::unique_ptr<WindowsCentralDatabase>>::failure(
                std::move(root).error());
        }
        auto store = Detail::WindowsDatabaseStore::open(
            root.value(),
            L"store.sqlite",
            L"store.sqlite.migration.lock",
            std::move(runtimeDiagnostics),
            std::move(clock),
            Detail::WindowsDatabaseStoreOptions{
                Migrations::DatabaseKind::Central,
                Detail::WinsqliteSynchronousMode::Full,
                false},
            context);
        if (!store) {
            return Domain::Result<std::unique_ptr<WindowsCentralDatabase>>::failure(
                std::move(store).error());
        }
        auto implementation = std::make_unique<Impl>(std::move(store).value());
        return Domain::Result<std::unique_ptr<WindowsCentralDatabase>>::success(
            std::unique_ptr<WindowsCentralDatabase>{
                new WindowsCentralDatabase{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<WindowsCentralDatabase>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The central Windows database could not be constructed."));
    }
}

Domain::Result<DatabaseSchemaSnapshot> WindowsCentralDatabase::schemaSnapshot(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<DatabaseSchemaSnapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The central database has no owned implementation."));
    }
    return implementation_->store->schemaSnapshot(context);
}

Domain::Result<void> WindowsCentralDatabase::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The central database has no owned implementation."));
    }
    return implementation_->store->quickCheck(context);
}

Domain::Result<DatabaseBackupReport> WindowsCentralDatabase::createOnlineBackup(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<DatabaseBackupReport>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The central database has no owned implementation."));
    }
    return implementation_->store->createOnlineBackup(context);
}

Domain::Result<void> WindowsCentralDatabase::close(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<void>::success();
    }
    return implementation_->store->close(context);
}

Detail::WindowsDatabaseStore* WindowsCentralDatabase::repositoryStore() noexcept
{
    return implementation_ ? implementation_->store.get() : nullptr;
}

} // namespace ForgeConductor::Persistence::Windows
