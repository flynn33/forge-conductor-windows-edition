#pragma once

#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

enum class ImageJobState { BeforeDispatch, Submitting, Queued, Running, Fetching, Completed, Failed, Cancelled, Unknown };

struct ImageJobRequest final {
    std::string prompt;
    std::string negativePrompt;
    std::string destination;
    std::optional<std::string> source;
    std::optional<std::string> mask;
    std::uint64_t seed{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t steps{20U};
    double cfg{7.0};
    double denoise{1.0};
    std::uint32_t previewMaxDimension{256U};
    std::uint32_t timeoutSeconds{1800U};
};

struct ImageJobScope final {
    std::string projectId;
    std::string clientId;
    std::vector<std::string> roots;
    std::vector<FileAccess> grants;
    std::vector<FileAccess> denials;
    std::uint64_t generation{};
};

struct ImageJobRecord final {
    std::string jobId;
    ImageJobScope scope;
    ImageProviderConfig provider;
    ImageJobRequest request;
    ImageJobState state{ImageJobState::BeforeDispatch};
    std::string promptId;
    std::string graphSha256;
    std::string graphJson;
    std::optional<std::string> sourceRgbaSha256;
    std::optional<std::string> maskRgbaSha256;
    std::optional<std::string> sourcePngSha256;
    std::optional<std::string> maskPngSha256;
    bool destinationExisted{};
    std::optional<std::string> destinationBeforeSha256;
    std::string remoteState{"not_submitted"};
    bool submissionAcknowledged{};
    bool publicationSuppressed{};
    bool cancellationRequested{};
    bool queueDeleteAccepted{};
    bool recovered{};
    std::int64_t createdUtcMilliseconds{};
    std::uint32_t ownerHostPid{};
    std::uint64_t ownerHostCreationTime{};
    bool ownerReleased{};
    std::optional<Error> error;
    std::optional<std::string> artifactSha256;
    std::optional<std::string> artifactRgbaSha256;
    std::uint64_t artifactBytes{};
    bool artifactPublished{};
};

} // namespace ForgeConductor::Domain
