#pragma once
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"
#include <memory>
#include <string>
namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
struct LMStudioChatControlActions;
class LMStudioChatContinuityAccess;
}
class WindowsLMStudioChatContinuity final {
public:
    WindowsLMStudioChatContinuity(Domain::ProjectId project, Domain::PathText projectRoot,
        Domain::PathText home, Domain::PathText lmStudioRoot, Domain::PathText executable,
        Domain::LocalModelConfig configuration, Contracts::ILegacyMemoryService& memory,
        Contracts::ILegacyContextContinuityService& continuity, Contracts::IProjectMemoryService& projectMemory,
        Contracts::IClock& clock, Contracts::IUuidGenerator& uuid, Contracts::IConfigurationStore& configurationStore,
        bool initialWorkspaceConfirmed = false);
    ~WindowsLMStudioChatContinuity();
    WindowsLMStudioChatContinuity(const WindowsLMStudioChatContinuity&) = delete;
    WindowsLMStudioChatContinuity& operator=(const WindowsLMStudioChatContinuity&) = delete;
    void start();
    void shutdown() noexcept;
    [[nodiscard]] std::string status() const;
    void recordTool(std::string_view name, bool succeeded, std::string_view payload);
    void bindWorkspace(const Domain::ProjectId& project, const Domain::PathText& projectRoot) noexcept;
private:
    friend class Detail::LMStudioChatContinuityAccess;
    WindowsLMStudioChatContinuity(Domain::ProjectId project, Domain::PathText projectRoot,
        Domain::PathText home, Domain::PathText lmStudioRoot, Domain::PathText executable,
        Domain::LocalModelConfig configuration, Contracts::ILegacyMemoryService& memory,
        Contracts::ILegacyContextContinuityService& continuity, Contracts::IProjectMemoryService& projectMemory,
        Contracts::IClock& clock, Contracts::IUuidGenerator& uuid, Contracts::IConfigurationStore& configurationStore,
        bool initialWorkspaceConfirmed, std::shared_ptr<Detail::LMStudioChatControlActions> controls);
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
