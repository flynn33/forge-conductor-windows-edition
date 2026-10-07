#pragma once

#include "ForgeConductor/Contracts/IScheduledTaskService.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include <cstddef>
#include <functional>
#include <memory>

namespace ForgeConductor::Application {
struct ScheduleStoragePaths final {
    Contracts::AuthorizedPath read;
    Contracts::AuthorizedPath write;
    Contracts::AuthorizedPath create;
};

// One Manager owns this file and dispatch loop. The storage callback supplies
// separate host-owned capabilities; user arguments never select its path.
class ScheduledTaskService final : public Contracts::IScheduledTaskService {
public:
    static constexpr std::size_t MaximumSchedules = 32U;
    static constexpr std::size_t MaximumTaskBytes = 64U * 1024U;
    static constexpr std::size_t MaximumStoreBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t MaximumResponseBytes = 128U * 1024U;
    static constexpr std::size_t MaximumRecordPageBytes = 32U * 1024U;
    using Paths = std::function<Domain::Result<ScheduleStoragePaths>(const Domain::OperationContext&)>;
    using Notification = std::function<Domain::Result<std::string>(std::string_view, const Domain::OperationContext&)>;
    ScheduledTaskService(Contracts::IManagedRunService&, Contracts::IWorkspaceAuthority&,
        Contracts::IAtomicFileStore&, Contracts::IClock&, Contracts::IUuidGenerator&,
        Contracts::IToolCatalog&, Paths, Notification = {});
    ~ScheduledTaskService() noexcept override;
    ScheduledTaskService(const ScheduledTaskService&) = delete;
    ScheduledTaskService& operator=(const ScheduledTaskService&) = delete;
    [[nodiscard]] Domain::Result<std::string> execute(std::string_view, std::string_view,
        const Contracts::WorkspaceAuthority&, const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<void> initialize(const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<void> tick(const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<void> start() noexcept override;
    void shutdown() noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
} // namespace ForgeConductor::Application
