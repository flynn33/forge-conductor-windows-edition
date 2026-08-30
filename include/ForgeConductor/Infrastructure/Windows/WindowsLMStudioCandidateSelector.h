#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

// A discovery candidate represents exactly one resource: either an application
// executable or a configuration file. Splitting the resources keeps evidence
// truthful when, for example, an installed executable exists but mcp.json is
// missing or malformed.
struct WindowsLMStudioDiscoveryCandidate final {
    Domain::LMStudioDiscoverySource source;
    Domain::PathText evidencePath;
    std::optional<Domain::PathText> applicationExecutable;
    std::optional<Domain::PathText> configurationPath;
    std::optional<Domain::PathText> installationRoot;
    std::optional<std::string> version;
    bool valid{};
    std::string detail;
};

struct WindowsLMStudioCandidateSelectorOptions final {
    static constexpr std::size_t DefaultMaximumCandidates = 64U;
    static constexpr std::size_t DefaultMaximumConfigurationBytes =
        2U * 1024U * 1024U;
    static constexpr std::size_t DefaultMaximumJsonDepth = 32U;

    std::size_t maximumCandidates{DefaultMaximumCandidates};
    std::size_t maximumConfigurationBytes{
        DefaultMaximumConfigurationBytes};
    std::size_t maximumJsonDepth{DefaultMaximumJsonDepth};
};

// Immutable evaluation retained beside the selected status. The original
// candidate remains available so a narrower composition layer can reject
// ambiguity or duplicate resources without re-running discovery.
class WindowsLMStudioCandidateEvaluation final {
public:
    [[nodiscard]] const WindowsLMStudioDiscoveryCandidate& candidate() const
        noexcept
    {
        return candidate_;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool selected() const noexcept { return selected_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    friend class WindowsLMStudioCandidateSelector;

    WindowsLMStudioCandidateEvaluation(
        WindowsLMStudioDiscoveryCandidate candidate,
        bool valid,
        bool selected,
        std::string detail);

    const WindowsLMStudioDiscoveryCandidate candidate_;
    const bool valid_;
    const bool selected_;
    const std::string detail_;
};

// Selection evidence is bound to the exact immutable authority identity used
// for configuration reads. Consumers can therefore reject stale or foreign
// evidence before issuing a narrower capability.
class WindowsLMStudioCandidateSelection final {
public:
    WindowsLMStudioCandidateSelection(
        const WindowsLMStudioCandidateSelection&) = default;
    WindowsLMStudioCandidateSelection(
        WindowsLMStudioCandidateSelection&&) noexcept = default;
    WindowsLMStudioCandidateSelection& operator=(
        const WindowsLMStudioCandidateSelection&) = delete;
    WindowsLMStudioCandidateSelection& operator=(
        WindowsLMStudioCandidateSelection&&) = delete;

    [[nodiscard]] const Domain::LMStudioEnvironmentStatus& status() const
        noexcept
    {
        return status_;
    }

    [[nodiscard]] const std::vector<WindowsLMStudioCandidateEvaluation>&
    evaluations() const noexcept
    {
        return evaluations_;
    }

    [[nodiscard]] const Domain::AuthorityId& authorityId() const noexcept
    {
        return authorityId_;
    }

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept
    {
        return projectId_;
    }

    [[nodiscard]] const Domain::ClientId& callerId() const noexcept
    {
        return callerId_;
    }

    [[nodiscard]] std::uint64_t authorityGeneration() const noexcept
    {
        return authorityGeneration_;
    }

private:
    friend class WindowsLMStudioCandidateSelector;

    WindowsLMStudioCandidateSelection(
        Domain::LMStudioEnvironmentStatus status,
        std::vector<WindowsLMStudioCandidateEvaluation> evaluations,
        Domain::AuthorityId authorityId,
        Domain::ProjectId projectId,
        Domain::ClientId callerId,
        std::uint64_t authorityGeneration);

    const Domain::LMStudioEnvironmentStatus status_;
    const std::vector<WindowsLMStudioCandidateEvaluation> evaluations_;
    const Domain::AuthorityId authorityId_;
    const Domain::ProjectId projectId_;
    const Domain::ClientId callerId_;
    const std::uint64_t authorityGeneration_;
};

// Synchronous, bounded selector shared by the environment service and Manager
// composition. It owns no filesystem, authority, thread, or native resource.
class WindowsLMStudioCandidateSelector final {
public:
    WindowsLMStudioCandidateSelector(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        WindowsLMStudioCandidateSelectorOptions options = {});

    WindowsLMStudioCandidateSelector(
        const WindowsLMStudioCandidateSelector&) = delete;
    WindowsLMStudioCandidateSelector& operator=(
        const WindowsLMStudioCandidateSelector&) = delete;
    WindowsLMStudioCandidateSelector(
        WindowsLMStudioCandidateSelector&&) = delete;
    WindowsLMStudioCandidateSelector& operator=(
        WindowsLMStudioCandidateSelector&&) = delete;

    [[nodiscard]] Domain::Result<WindowsLMStudioCandidateSelection> select(
        std::vector<WindowsLMStudioDiscoveryCandidate> candidates,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept;

private:
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    Contracts::IFileSystem& fileSystem_;
    const WindowsLMStudioCandidateSelectorOptions options_;
    std::optional<Domain::Error> constructionError_;
};

} // namespace ForgeConductor::Infrastructure::Windows
