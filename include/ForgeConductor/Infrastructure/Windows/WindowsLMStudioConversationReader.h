#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
    std::size_t messageIndex{};
    std::size_t selectedVersion{};
};

struct LMStudioNativeUserMessage final {
    std::string text;
    std::size_t messageIndex{};
    std::size_t selectedVersion{};
    // Conservative exclusion of the reserved message prefixes emitted by
    // Forge continuity control; false does not attest human authorship.
    bool forgeGenerated{};
};

struct LMStudioConversationObservation final {
    std::string conversationId;
    std::string conversationPath;
    std::string projectIdentifier;
    std::uint64_t usedTokens{};
    std::uint64_t contextCapacity{};
    // LM Studio's cached full rendered prompt count, admitted only for the
    // same model instance and capacity as the selected provider observation.
    // Refreshed before/after outer predictions; not current KV usage.
    std::optional<std::uint64_t> cachedRenderedPromptTokens;
    bool overflow{};
    bool toolsActive{};
    std::string stopReason;
    // Stable internal comparison token for the latest selected provider generation.
    std::string generationEvidence;
    std::vector<std::string> plugins;
    std::vector<std::string> userMessages;
    std::vector<LMStudioNativeUserMessage> userMessageEvidence;
    std::vector<LMStudioNativeToolResult> nativeToolResults;
};

// Reads LM Studio's selected, persisted conversation without changing it.
// Provider usage comes from the latest selected generation's statistics.
// The separate native tokenCount cache projects the full rendered prompt.
class WindowsLMStudioConversationReader final {
public:
    [[nodiscard]] static Domain::Result<
        std::optional<LMStudioConversationObservation>> read(
        const Domain::PathText& lmStudioRoot,
        const Domain::OperationContext& context) noexcept;
    // Reads an exact saved predecessor without selecting or modifying it.
    [[nodiscard]] static Domain::Result<
        std::optional<LMStudioConversationObservation>> readConversation(
        const Domain::PathText& lmStudioRoot,
        std::string_view conversationId,
        const Domain::OperationContext& context) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
