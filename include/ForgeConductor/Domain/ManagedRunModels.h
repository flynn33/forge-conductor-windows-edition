#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumManagedRunTaskBytes = 128U * 1024U;
inline constexpr std::size_t MaximumManagedRunOutputBytes = 256U * 1024U;
inline constexpr std::uint32_t DefaultReviewerReceiveTimeoutSeconds = 600U;
inline constexpr std::uint32_t MaximumManagedProviderReceiveTimeoutSeconds = 3600U;
inline constexpr std::size_t MaximumReviewerOpeningMessageBytes = 64U * 1024U;
inline constexpr std::size_t MaximumManagedImagePreviewBase64Bytes = 512U * 1024U;

enum class ManagedRunState {
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Paused
};

enum class ManagedRunEvidenceIntegrity {
    NotTerminal,
    LegacyUnsealed,
    Verified,
    Mismatch
};

struct ManagedNativeTaskCheck final {
    Sha256Digest commandDigest;
    Sha256Digest stdoutDigest;
    Sha256Digest stderrDigest;
    int exitCode{};
    bool passed{};
    bool timedOut{};
    bool cancelled{};
    bool terminationConfirmed{};
    std::uint64_t elapsedMilliseconds{};
    UtcTimePoint checkedAt;
};

struct ManagedFunctionCall final {
    std::string callId;
    std::string name;
    std::string canonicalArguments;
};

struct ManagedImagePreview final {
    std::string mimeType;
    std::string base64Data;
};

[[nodiscard]] inline bool isValidManagedImagePreview(const ManagedImagePreview& preview) noexcept
{
    const std::string_view encoded{preview.base64Data};
    if (preview.mimeType != "image/png" || encoded.size() > MaximumManagedImagePreviewBase64Bytes ||
        encoded.size() % 4U != 0U || !encoded.starts_with("iVBORw0KGgo")) return false;
    const auto firstPadding = encoded.find('=');
    const auto dataEnd = firstPadding == std::string_view::npos ? encoded.size() : firstPadding;
    const auto padding = encoded.size() - dataEnd;
    if (padding > 2U) return false;
    for (std::size_t index{}; index < dataEnd; ++index) {
        const char value = encoded[index];
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '+' || value == '/')) return false;
    }
    for (std::size_t index = dataEnd; index < encoded.size(); ++index)
        if (encoded[index] != '=') return false;
    if (padding != 0U) {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto tail = alphabet.find(encoded[dataEnd - 1U]);
        if ((padding == 1U && (tail & 3U) != 0U) || (padding == 2U && (tail & 15U) != 0U)) return false;
    }
    return true;
}

struct ManagedFunctionCallOutput final {
    std::string callId;
    std::string canonicalOutput;
    std::optional<ManagedImagePreview> image;
};

struct ManagedProviderTurnRequest final {
    ProjectId projectId;
    SessionId runId;
    std::uint64_t authorityGeneration{};
    std::string input;
    std::optional<ProviderSessionId> previousResponseId;
    std::vector<McpToolDescriptor> tools;
    std::vector<ManagedFunctionCallOutput> toolOutputs;
    std::optional<std::uint32_t> providerReceiveTimeoutSeconds;
};

struct ManagedProviderTurnResult final {
    ProviderSessionId responseId;
    std::string outputText;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
    std::vector<ManagedFunctionCall> functionCalls;
};

// Captured from an authenticated caller capability, never from public tool args.
// A worker may retain or narrow this scope; its owner must not widen it later.
struct ManagedRunWorkerScope final {
    std::vector<PathText> trustedRoots;
    std::vector<FileAccess> grants;
    std::vector<FileAccess> denials;
    bool shellEnabled{};
    std::vector<std::string> allowedTools;
    std::uint32_t timeoutSeconds{600U};
    bool operator==(const ManagedRunWorkerScope&) const = default;
};

struct ManagedRunRecord final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    std::string task;
    std::uint64_t authorityGeneration{};
    ManagedRunState state{ManagedRunState::Running};
    std::optional<ProviderSessionId> providerResponseId;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
    std::optional<std::string> outputText;
    std::optional<Error> lastError;
    std::vector<ManagedFunctionCall> pendingFunctionCalls;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
    bool allowTools{true};
    std::optional<Sha256Digest> evidenceSeal;
    ManagedRunEvidenceIntegrity evidenceIntegrity{
        ManagedRunEvidenceIntegrity::NotTerminal};
    std::vector<ManagedNativeTaskCheck> nativeTaskChecks;
    bool readOnlyTools{};
    bool outputTruncated{};
    std::optional<std::uint32_t> providerReceiveTimeoutSeconds;
    std::optional<ManagedRunWorkerScope> workerScope;
    bool workerInterrupted{};
};

struct ManagedRunStartRequest final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    OperationId operationId;
    CorrelationId correlationId;
    std::uint64_t authorityGeneration{};
    std::string task;
    bool allowTools{true};
    bool automaticContinuity{true};
    bool readOnlyTools{};
    std::optional<std::uint32_t> providerReceiveTimeoutSeconds;
    std::optional<ManagedRunWorkerScope> workerScope;
};

struct ManagedRunSnapshot final {
    ManagedRunRecord record;
    bool managerOwned{true};
    bool cancellationRequested{};
    bool pauseRequested{};
};

} // namespace ForgeConductor::Domain
