#pragma once

#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatContinuity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatControl.h"
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct LMStudioChatControlActions final {
    std::function<Domain::Result<void>(const Domain::PathText&, const Domain::OperationContext&)> activate;
    std::function<Domain::Result<bool>(const Domain::PathText&, const Domain::OperationContext&)> idle;
    std::function<Domain::Result<bool>(const Domain::PathText&, std::string_view, const Domain::OperationContext&)> pause;
    std::function<Domain::Result<void>(const Domain::PathText&, std::string_view, bool,
        const Domain::OperationContext&, std::optional<std::string_view>,
        std::function<void(std::string_view)>, LMStudioChatEffectObserver)> send;

    Domain::Result<void> sendMessage(const Domain::PathText& executable, std::string_view text,
        bool newChat, const Domain::OperationContext& context,
        std::optional<std::string_view> expectedConversation = std::nullopt,
        std::function<void(std::string_view)> successorCreated = {},
        LMStudioChatEffectObserver effectObserver = {}) const
    {
        return send(executable, text, newChat, context, expectedConversation,
            std::move(successorCreated), std::move(effectObserver));
    }
};

class LMStudioChatContinuityAccess final {
public:
    template <typename... Arguments>
    static std::unique_ptr<WindowsLMStudioChatContinuity> create(
        std::shared_ptr<LMStudioChatControlActions> controls, Arguments&&... arguments)
    {
        return std::unique_ptr<WindowsLMStudioChatContinuity>{new WindowsLMStudioChatContinuity{
            std::forward<Arguments>(arguments)..., std::move(controls)}};
    }
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
