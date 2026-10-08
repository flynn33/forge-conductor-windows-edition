#pragma once
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatContinuity.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"
#include "ManagerTransitionWorker.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace ForgeConductor::Hosts::Manager
{
namespace Detail
{
class ManagerVisibleChatContinuityAccess;
}
// Owns the native observer across individual authenticated pipe connections.
class ManagerVisibleChatContinuity final : public IManagerTransitionWorker
{
public:
    ManagerVisibleChatContinuity(Domain::PathText home, Domain::PathText studio,
                                 Domain::PathText lmExecutable, Domain::PathText forgeExecutable,
                                 Contracts::IProjectRegistryRepository& registry,
                                 Contracts::IWorkspaceAuthority& authority,
                                 Contracts::ILegacyMemoryService& memory,
                                 Contracts::ILegacyContextContinuityService& continuity,
                                 Contracts::IProjectMemoryService& projects,
                                 Contracts::IClock& clock, Contracts::IUuidGenerator& uuid,
                                 Contracts::IConfigurationStore& configuration);
    ~ManagerVisibleChatContinuity() noexcept override;
    [[nodiscard]] Domain::Result<void> start() noexcept override;
    void beginShutdown() noexcept override;
    void shutdown() noexcept override;
    [[nodiscard]] Domain::Result<ForgeConductor::Manager::ManagerVisibleChatSnapshot> observe(
        const ForgeConductor::Manager::ManagerVisibleChatObserveRequest&,
        const Domain::OperationContext&) noexcept;
    [[nodiscard]] Domain::Result<ForgeConductor::Manager::ManagerVisibleChatSnapshot> status(
        const Domain::ProjectId&, const Domain::OperationContext&) noexcept;

private:
    friend class Detail::ManagerVisibleChatContinuityAccess;
    using ObserverFactory =
        std::function<std::shared_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity>(
            const Domain::ProjectId&, const Domain::PathText&, const Domain::LocalModelConfig&)>;
    ObserverFactory observerFactory_;
    [[nodiscard]] Domain::Result<void> activate(const Domain::ProjectId&, const Domain::PathText&,
                                                const Domain::OperationContext&);
    [[nodiscard]] Domain::Result<void> validate(const Domain::ProjectId&, const Domain::PathText&,
                                                const Domain::OperationContext&);
    [[nodiscard]] Domain::OperationContext context();
    [[nodiscard]] ForgeConductor::Manager::ManagerVisibleChatSnapshot snapshot(
        const Domain::ProjectId&) const;
    Domain::PathText home_, studio_, lmExecutable_, forgeExecutable_;
    Contracts::IProjectRegistryRepository& registry_;
    Contracts::IWorkspaceAuthority& authority_;
    Contracts::ILegacyMemoryService& memory_;
    Contracts::ILegacyContextContinuityService& continuity_;
    Contracts::IProjectMemoryService& projects_;
    Contracts::IClock& clock_;
    Contracts::IUuidGenerator& uuid_;
    Contracts::IConfigurationStore& configuration_;
    std::mutex mutex_;
    std::shared_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity> observer_;
    std::atomic<std::shared_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity>>
        publishedObserver_;
    std::optional<Domain::ProjectId> project_;
    std::optional<Domain::PathText> root_;
    bool running_{};
    std::atomic<bool> stopping_{};
    std::string startupError_;
};
// The composition retains a shared owner for dispatcher callbacks and supplies
// this lifecycle adapter to the existing process worker group.
class ManagerVisibleChatWorker final : public IManagerTransitionWorker
{
public:
    explicit ManagerVisibleChatWorker(std::shared_ptr<ManagerVisibleChatContinuity> owner)
        : owner_{std::move(owner)}
    {
    }
    Domain::Result<void> start() noexcept override
    {
        return owner_->start();
    }
    void beginShutdown() noexcept override
    {
        owner_->beginShutdown();
    }
    void shutdown() noexcept override
    {
        owner_->shutdown();
    }

private:
    std::shared_ptr<ManagerVisibleChatContinuity> owner_;
};
} // namespace ForgeConductor::Hosts::Manager
