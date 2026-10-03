#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

// Exact tool-result evidence persisted by the native conversation renderer.
// content is LM Studio's saved string; textBodies decode its MCP text blocks.
struct LMStudioNativeToolResult final {
    std::string name;
    std::string content;
    std::string pluginIdentifier;
    std::string requestId;
    std::vector<std::string> textBodies;
};

struct LMStudioConversationObservation final {
    std::string conversationId;
    std::string conversationPath;
    std::string projectIdentifier;
    std::uint64_t usedTokens{};
    std::uint64_t contextCapacity{};
    bool overflow{};
    bool toolsActive{};
    std::string stopReason;
    // Stable internal comparison token for the latest selected provider generation.
    std::string generationEvidence;
    std::vector<std::string> plugins;
    std::vector<std::string> userMessages;
    std::vector<LMStudioNativeToolResult> nativeToolResults;
};

// Reads LM Studio's selected, persisted conversation without changing it.
// Token observations come from the selected generation's provider statistics,
// not the conversation list's aggregate tokenCount.
class WindowsLMStudioConversationReader final {
public:
    [[nodiscard]] static Domain::Result<
        std::optional<LMStudioConversationObservation>> read(
        const Domain::PathText& lmStudioRoot,
        const Domain::OperationContext& context) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
