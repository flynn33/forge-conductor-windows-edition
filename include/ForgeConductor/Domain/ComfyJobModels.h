#pragma once

#include "ForgeConductor/Domain/ConfigurationModels.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

enum class ComfyJobState {
    Preparing, BeforeDispatch, Submitting, Queued, Running, Fetching,
    AwaitingPreviewApproval, Completed, Failed, Cancelled, Unknown
};

struct ComfyArtifact final {
    std::string nodeId;
    std::string path;
    std::string mediaType;
    std::string sha256;
    std::uint64_t bytes{};
    std::string metadataJson;
};

struct ComfyRenderPlan final {
    std::string planId;
    std::uint64_t revision{1};
    std::string previewGraphJson;
    std::string finalGraphJson;
    std::string finalGraphSha256;
    std::string conversationId;
    std::string conversationPrefixSha256;
    std::uint64_t approvalBoundary{};
    bool approved{};
};

struct ComfyJobRecord final {
    std::string jobId;
    std::string projectId;
    std::string kind;
    std::string operation;
    ComfyJobState state{ComfyJobState::BeforeDispatch};
    std::string promptId;
    std::string graphSha256;
    std::uint64_t downloadedBytes{};
    std::vector<ComfyArtifact> artifacts;
    std::optional<ComfyRenderPlan> renderPlan;
};

} // namespace ForgeConductor::Domain
