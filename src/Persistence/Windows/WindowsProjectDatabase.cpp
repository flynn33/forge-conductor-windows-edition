#include "ForgeConductor/Persistence/Windows/WindowsProjectDatabase.h"

#include "Detail/WindowsDatabaseStore.h"

#include <memory>
#include <utility>

namespace ForgeConductor::Persistence::Windows {

struct WindowsProjectDatabase::Impl final {
    Impl(
        Domain::ProjectId value,
        std::unique_ptr<Detail::WindowsDatabaseStore> database) noexcept
        : projectId{std::move(value)}, store{std::move(database)}
    {
    }

    Domain::ProjectId projectId;
    std::unique_ptr<Detail::WindowsDatabaseStore> store;
};

WindowsProjectDatabase::WindowsProjectDatabase(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsProjectDatabase::~WindowsProjectDatabase() noexcept = default;

Domain::Result<std::unique_ptr<WindowsProjectDatabase>> WindowsProjectDatabase::open(
    const Domain::ProjectId& projectId,
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    const WindowsProjectDatabaseOptions options,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!applicationPaths) {
            return Domain::Result<std::unique_ptr<WindowsProjectDatabase>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The project database requires application-owned path resolution."));
        }
        auto root = applicationPaths->projectRoot(projectId, context);
        if (!root) {
            return Domain::Result<std::unique_ptr<WindowsProjectDatabase>>::failure(
                std::move(root).error());
        }
        auto store = Detail::WindowsDatabaseStore::open(
            root.value(),
            L"memory.sqlite",
            L"memory.sqlite.migration.lock",
            std::move(runtimeDiagnostics),
            std::move(clock),
            Detail::WindowsDatabaseStoreOptions{
                Migrations::DatabaseKind::Project,
                Detail::WinsqliteSynchronousMode::Normal,
                options.enableFts5},
            context);
        if (!store) {
            return Domain::Result<std::unique_ptr<WindowsProjectDatabase>>::failure(
                std::move(store).error());
        }
        auto implementation = std::make_unique<Impl>(
            projectId, std::move(store).value());
        return Domain::Result<std::unique_ptr<WindowsProjectDatabase>>::success(
            std::unique_ptr<WindowsProjectDatabase>{
                new WindowsProjectDatabase{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<WindowsProjectDatabase>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The project Windows database could not be constructed."));
    }
}

const Domain::ProjectId& WindowsProjectDatabase::projectId() const noexcept
{
    return implementation_->projectId;
}

Domain::Result<DatabaseSchemaSnapshot> WindowsProjectDatabase::schemaSnapshot(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<DatabaseSchemaSnapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project database has no owned implementation."));
    }
    return implementation_->store->schemaSnapshot(context);
}

Domain::Result<void> WindowsProjectDatabase::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project database has no owned implementation."));
    }
    return implementation_->store->quickCheck(context);
}

Domain::Result<DatabaseBackupReport> WindowsProjectDatabase::createOnlineBackup(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<DatabaseBackupReport>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project database has no owned implementation."));
    }
    return implementation_->store->createOnlineBackup(context);
}

Domain::Result<void> WindowsProjectDatabase::close(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->store) {
        return Domain::Result<void>::success();
    }
    return implementation_->store->close(context);
}

Detail::WindowsDatabaseStore* WindowsProjectDatabase::repositoryStore() noexcept
{
    return implementation_ ? implementation_->store.get() : nullptr;
}

} // namespace ForgeConductor::Persistence::Windows
