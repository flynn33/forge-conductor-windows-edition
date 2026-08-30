#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioCandidateSelector.h"

#include "Detail/OperationContextGuard.h"
#include "Detail/UtfConversion.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess candidate) noexcept
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

[[nodiscard]] bool isCandidateLocalReadError(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::RecordNotFound ||
        error.code == Domain::ErrorCodes::Unauthorized ||
        error.code == Domain::ErrorCodes::PathOutsideAuthority ||
        error.code == Domain::ErrorCodes::InvalidRequest ||
        error.code == Domain::ErrorCodes::PayloadTooLarge;
}

[[nodiscard]] std::string appendDetail(
    std::string detail,
    const std::string_view addition)
{
    if (addition.empty()) {
        return detail;
    }
    if (!detail.empty()) {
        detail.append(" ");
    }
    detail.append(addition);
    return detail;
}

[[nodiscard]] Domain::Result<Domain::PathText> pathText(
    const std::wstring_view path)
{
    auto utf8 = Detail::strictUtf16ToUtf8(path);
    if (!utf8) {
        return Domain::Result<Domain::PathText>::failure(
            std::move(utf8).error());
    }
    return Domain::PathText::create(utf8.value());
}

[[nodiscard]] std::optional<Domain::PathText> parentPath(
    const Domain::PathText& child) noexcept
{
    try {
        auto converted = Detail::strictUtf8ToUtf16(child.value());
        if (!converted) {
            return std::nullopt;
        }
        const auto parent =
            std::filesystem::path{converted.value()}.parent_path();
        if (parent.empty()) {
            return std::nullopt;
        }
        auto encoded = pathText(parent.native());
        return encoded
            ? std::optional<Domain::PathText>{std::move(encoded).value()}
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] int sourcePriority(
    const Domain::LMStudioDiscoverySource source) noexcept
{
    switch (source) {
    case Domain::LMStudioDiscoverySource::ExplicitConfiguration:
        return 0;
    case Domain::LMStudioDiscoverySource::InstalledApplication:
        return 1;
    case Domain::LMStudioDiscoverySource::KnownUserLocation:
        return 2;
    case Domain::LMStudioDiscoverySource::RunningProcess:
        return 3;
    }
    return 4;
}

struct ConfigurationValidation final {
    bool valid{};
    std::string detail;
};

[[nodiscard]] Domain::Result<ConfigurationValidation> parseConfiguration(
    const std::vector<std::byte>& bytes,
    const std::size_t maximumDepth) noexcept
{
    try {
        if (bytes.empty()) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{
                    false, "The configuration file is empty."});
        }
        const std::string text{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        std::vector<std::unordered_set<std::string>> objectKeys;
        bool rejectedDepth{};
        bool rejectedDuplicate{};
        const auto callback =
            [&](const int depth, const Json::parse_event_t event, Json& value) {
                if (depth < 0 ||
                    static_cast<std::size_t>(depth) > maximumDepth) {
                    rejectedDepth = true;
                    return false;
                }
                if (event == Json::parse_event_t::object_start) {
                    objectKeys.emplace_back();
                } else if (event == Json::parse_event_t::key) {
                    if (objectKeys.empty() ||
                        !objectKeys.back()
                             .insert(value.get<std::string>())
                             .second) {
                        rejectedDuplicate = true;
                        return false;
                    }
                } else if (
                    event == Json::parse_event_t::object_end &&
                    !objectKeys.empty()) {
                    objectKeys.pop_back();
                }
                return true;
            };
        const auto document = Json::parse(text, callback, false, false);
        if (rejectedDepth) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{
                    false,
                    "The configuration JSON exceeds the bounded nesting "
                    "depth."});
        }
        if (rejectedDuplicate) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{
                    false,
                    "The configuration JSON contains a duplicate object "
                    "key."});
        }
        if (document.is_discarded()) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{
                    false,
                    "The configuration file is not valid strict JSON."});
        }
        if (!document.is_object()) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{
                    false,
                    "The configuration JSON root is not an object."});
        }
        const auto servers = document.find("mcpServers");
        if (servers != document.end()) {
            if (!servers->is_object()) {
                return Domain::Result<ConfigurationValidation>::success(
                    ConfigurationValidation{
                        false,
                        "The configuration mcpServers member is not an "
                        "object."});
            }
            for (const auto& [name, server] : servers->items()) {
                static_cast<void>(name);
                if (!server.is_object()) {
                    return Domain::Result<ConfigurationValidation>::success(
                        ConfigurationValidation{
                            false,
                            "An MCP server registration is not a JSON "
                            "object."});
                }
            }
        }
        return Domain::Result<ConfigurationValidation>::success(
            ConfigurationValidation{
                true,
                "The configuration file parsed as bounded strict JSON."});
    } catch (...) {
        return Domain::Result<ConfigurationValidation>::success(
            ConfigurationValidation{
                false,
                "The configuration file could not be parsed as strict "
                "JSON."});
    }
}

[[nodiscard]] Domain::Result<ConfigurationValidation> validateConfiguration(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    const Domain::PathText& candidate,
    const Contracts::WorkspaceAuthority& readAuthority,
    const WindowsLMStudioCandidateSelectorOptions& options,
    const Domain::OperationContext& context)
{
    auto authorized = workspaceAuthority.authorize(
        readAuthority,
        Domain::PathAuthorizationRequest{
            candidate, std::nullopt, Domain::FileAccess::Read, false},
        context);
    if (!authorized) {
        if (!isCandidateLocalReadError(authorized.error())) {
            return Domain::Result<ConfigurationValidation>::failure(
                std::move(authorized).error());
        }
        return Domain::Result<ConfigurationValidation>::success(
            ConfigurationValidation{
                false,
                "Read authority rejected the configuration candidate (" +
                    authorized.error().code + ")."});
    }
    auto content = fileSystem.readFile(
        authorized.value(), options.maximumConfigurationBytes, context);
    if (!content) {
        if (!isCandidateLocalReadError(content.error())) {
            return Domain::Result<ConfigurationValidation>::failure(
                std::move(content).error());
        }
        return Domain::Result<ConfigurationValidation>::success(
            ConfigurationValidation{
                false,
                "The configuration candidate could not be read (" +
                    content.error().code + ")."});
    }
    return parseConfiguration(content.value(), options.maximumJsonDepth);
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

} // namespace

WindowsLMStudioCandidateEvaluation::WindowsLMStudioCandidateEvaluation(
    WindowsLMStudioDiscoveryCandidate candidate,
    const bool valid,
    const bool selected,
    std::string detail)
    : candidate_{std::move(candidate)},
      valid_{valid},
      selected_{selected},
      detail_{std::move(detail)}
{
}

WindowsLMStudioCandidateSelection::WindowsLMStudioCandidateSelection(
    Domain::LMStudioEnvironmentStatus status,
    std::vector<WindowsLMStudioCandidateEvaluation> evaluations,
    Domain::AuthorityId authorityId,
    Domain::ProjectId projectId,
    Domain::ClientId callerId,
    const std::uint64_t authorityGeneration)
    : status_{std::move(status)},
      evaluations_{std::move(evaluations)},
      authorityId_{std::move(authorityId)},
      projectId_{std::move(projectId)},
      callerId_{std::move(callerId)},
      authorityGeneration_{authorityGeneration}
{
}

WindowsLMStudioCandidateSelector::WindowsLMStudioCandidateSelector(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    WindowsLMStudioCandidateSelectorOptions options)
    : workspaceAuthority_{workspaceAuthority},
      fileSystem_{fileSystem},
      options_{std::move(options)}
{
    if (options_.maximumCandidates == 0U ||
        options_.maximumCandidates > 256U ||
        options_.maximumConfigurationBytes == 0U ||
        options_.maximumConfigurationBytes >
            WindowsLMStudioCandidateSelectorOptions::
                DefaultMaximumConfigurationBytes ||
        options_.maximumJsonDepth == 0U ||
        options_.maximumJsonDepth > 128U) {
        constructionError_ = Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "LM Studio candidate selector bounds are outside their supported "
            "ranges.");
    }
}

Domain::Result<WindowsLMStudioCandidateSelection>
WindowsLMStudioCandidateSelector::select(
    std::vector<WindowsLMStudioDiscoveryCandidate> candidates,
    const Contracts::WorkspaceAuthority& readAuthority,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = validateContext(
            context, "select bounded LM Studio discovery candidates");
        if (!active) {
            return Domain::Result<WindowsLMStudioCandidateSelection>::failure(
                std::move(active).error());
        }
        if (constructionError_) {
            return Domain::Result<WindowsLMStudioCandidateSelection>::failure(
                *constructionError_);
        }
        if (!containsAccess(
                readAuthority.grants(), Domain::FileAccess::Read) ||
            containsAccess(
                readAuthority.denials(), Domain::FileAccess::Read)) {
            return Domain::Result<WindowsLMStudioCandidateSelection>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "LM Studio candidate selection requires an authority that "
                    "grants read access."));
        }
        if (candidates.size() > options_.maximumCandidates) {
            return Domain::Result<WindowsLMStudioCandidateSelection>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "LM Studio environment inspection exceeded its candidate "
                    "bound."));
        }

        std::stable_sort(
            candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
                return sourcePriority(left.source) <
                    sourcePriority(right.source);
            });

        Domain::LMStudioEnvironmentStatus status;
        status.discoveryEvidence.reserve(candidates.size());
        std::vector<WindowsLMStudioCandidateEvaluation> evaluations;
        evaluations.reserve(candidates.size());
        for (auto& candidate : candidates) {
            active = validateContext(
                context, "continue LM Studio environment inspection");
            if (!active) {
                return Domain::Result<
                    WindowsLMStudioCandidateSelection>::failure(
                    std::move(active).error());
            }
            bool valid = candidate.valid;
            bool selected{};
            std::string detail = candidate.detail;
            const std::size_t resourceCount =
                static_cast<std::size_t>(
                    candidate.applicationExecutable.has_value()) +
                static_cast<std::size_t>(
                    candidate.configurationPath.has_value());
            if (candidate.valid && resourceCount != 1U) {
                valid = false;
                detail = appendDetail(
                    std::move(detail),
                    "The source candidate did not identify exactly one "
                    "resource.");
            }
            if (candidate.configurationPath && candidate.valid &&
                resourceCount == 1U) {
                auto validation = validateConfiguration(
                    workspaceAuthority_, fileSystem_,
                    *candidate.configurationPath, readAuthority, options_,
                    context);
                if (!validation) {
                    return Domain::Result<
                        WindowsLMStudioCandidateSelection>::failure(
                        std::move(validation).error());
                }
                valid = validation.value().valid;
                detail = appendDetail(
                    std::move(detail), validation.value().detail);
                if (valid && !status.configurationPath) {
                    status.configurationPath = candidate.configurationPath;
                    selected = true;
                }
            }
            if (candidate.applicationExecutable && valid &&
                !status.applicationExecutable) {
                status.applicationExecutable =
                    candidate.applicationExecutable;
                status.installationRoot = candidate.installationRoot
                    ? candidate.installationRoot
                    : parentPath(*candidate.applicationExecutable);
                status.version = candidate.version;
                selected = true;
            }
            status.discoveryEvidence.emplace_back(
                Domain::LMStudioDiscoveryEvidence{
                    candidate.source,
                    candidate.evidencePath,
                    valid,
                    selected,
                    detail});
            evaluations.push_back(WindowsLMStudioCandidateEvaluation{
                std::move(candidate), valid, selected, std::move(detail)});
        }
        status.lmStudioPresent = status.applicationExecutable.has_value();

        return Domain::Result<WindowsLMStudioCandidateSelection>::success(
            WindowsLMStudioCandidateSelection{
                std::move(status),
                std::move(evaluations),
                readAuthority.authorityId(),
                readAuthority.projectId(),
                readAuthority.callerId(),
                readAuthority.generation()});
    } catch (...) {
        return Domain::Result<WindowsLMStudioCandidateSelection>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Windows LM Studio candidate selection failed."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
