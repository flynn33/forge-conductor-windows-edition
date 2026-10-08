#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <functional>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

enum class LMStudioChatEffect { NewChat, Send };
enum class LMStudioChatEffectStage { BeforeDispatch, Confirmed };
struct LMStudioChatEffectReceipt final {
    LMStudioChatEffect effect;
    LMStudioChatEffectStage stage;
    std::string conversationId;
    std::size_t previousUserMessages{};
};
using LMStudioChatEffectObserver = std::function<Domain::Result<void>(const LMStudioChatEffectReceipt&)>;

class WindowsLMStudioChatControl final {
public:
    [[nodiscard]] static Domain::Result<void> activate(
        const Domain::PathText& executable,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<bool> idle(
        const Domain::PathText& executable,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<bool> pauseAtToolBoundary(
        const Domain::PathText& executable,
        std::string_view expectedConversationId,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<void> closeWindow(
        const Domain::PathText& executable,
        const Domain::OperationContext& context,
        std::optional<std::string_view> expectedConversationId = std::nullopt) noexcept;

    [[nodiscard]] static Domain::Result<void> send(
        const Domain::PathText& executable,
        std::string_view text,
        bool newChat,
        const Domain::OperationContext& context,
        std::optional<std::string_view> expectedConversationId = std::nullopt,
        std::function<void(std::string_view)> successorCreated = {},
        LMStudioChatEffectObserver effectObserver = {}) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
