#pragma once

#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Application {

class ProjectMemoryRepositoryCache final
    : public Contracts::IProjectMemoryRepositoryFactory,
      public Contracts::IContinuityRepositoryFactory {
public:
    ProjectMemoryRepositoryCache(
        std::shared_ptr<Contracts::IProjectMemoryRepositoryOpener> opener,
        std::size_t maximumOpenRepositories);
    ~ProjectMemoryRepositoryCache() override;

    ProjectMemoryRepositoryCache(const ProjectMemoryRepositoryCache&) = delete;
    ProjectMemoryRepositoryCache& operator=(const ProjectMemoryRepositoryCache&) = delete;
    ProjectMemoryRepositoryCache(ProjectMemoryRepositoryCache&&) = delete;
    ProjectMemoryRepositoryCache& operator=(ProjectMemoryRepositoryCache&&) = delete;

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectMemoryRepository>>
    open(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IContinuityRepository>>
    openContinuity(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] std::size_t openCount() const noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
