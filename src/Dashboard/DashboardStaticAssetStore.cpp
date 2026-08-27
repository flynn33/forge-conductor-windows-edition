#include "ForgeConductor/Dashboard/DashboardStaticAssetStore.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view NoStore = "no-store";
constexpr std::string_view HtmlMimeType = "text/html; charset=utf-8";

[[nodiscard]] Domain::Error error(
    const std::string_view code,
    const std::string_view message)
{
    return Domain::makeError(code, std::string{message});
}

[[nodiscard]] Domain::Error internalError()
{
    return error(
        Domain::ErrorCodes::InternalFailure,
        "Dashboard static asset storage failed safely.");
}

[[nodiscard]] std::optional<std::size_t> shellIndex(
    const DashboardShellAssetId identifier) noexcept
{
    switch (identifier) {
    case DashboardShellAssetId::Root:
        return 0U;
    case DashboardShellAssetId::Index:
        return 1U;
    case DashboardShellAssetId::Control:
        return 2U;
    case DashboardShellAssetId::Manager:
        return 3U;
    }
    return std::nullopt;
}

} // namespace

DashboardStaticAssetStore::DashboardStaticAssetStore(
    std::vector<Entry> assets,
    ShellAssets shellAssets,
    const std::size_t aggregateAssetBytes) noexcept
    : assets_{std::move(assets)},
      shellAssets_{std::move(shellAssets)},
      aggregateAssetBytes_{aggregateAssetBytes}
{
}

Domain::Result<std::unique_ptr<DashboardStaticAssetStore>>
DashboardStaticAssetStore::create(
    std::vector<DashboardStaticAssetDefinition> assets,
    std::vector<DashboardShellAssetMapping> shellMappings) noexcept
{
    using StoreResult =
        Domain::Result<std::unique_ptr<DashboardStaticAssetStore>>;
    try {
        if (assets.empty()) {
            return StoreResult::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard static asset store requires at least one asset."));
        }
        if (assets.size() > MaximumAssetCount) {
            return StoreResult::failure(error(
                Domain::ErrorCodes::LimitExceeded,
                "The dashboard static asset count exceeds 128."));
        }
        if (shellMappings.size() > MaximumShellAssetMappings) {
            return StoreResult::failure(error(
                Domain::ErrorCodes::LimitExceeded,
                "The dashboard shell mapping count exceeds four."));
        }

        std::sort(
            assets.begin(),
            assets.end(),
            [](const DashboardStaticAssetDefinition& left,
               const DashboardStaticAssetDefinition& right) noexcept {
                return left.path.relativePath() < right.path.relativePath();
            });

        std::size_t aggregateBytes{};
        for (std::size_t index{}; index < assets.size(); ++index) {
            const auto& asset = assets[index];
            if (index != 0U &&
                assets[index - 1U].path.relativePath() ==
                    asset.path.relativePath()) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::Conflict,
                    "A canonical dashboard static asset path is duplicated."));
            }
            if (asset.bytes.empty()) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard static assets may not be empty."));
            }
            if (asset.bytes.size() > MaximumAssetBodyBytes) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "A dashboard static asset exceeds 2,080,768 bytes."));
            }
            if (asset.bytes.size() >
                MaximumAggregateAssetBytes - aggregateBytes) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::LimitExceeded,
                    "Dashboard static assets exceed the 8 MiB aggregate limit."));
            }
            aggregateBytes += asset.bytes.size();
        }

        std::vector<Entry> entries;
        entries.reserve(assets.size());
        for (auto& definition : assets) {
            auto asset = DashboardStaticAssetHandle{new DashboardStaticAsset{
                std::move(definition.bytes),
                std::string{definition.path.mimeType()},
                std::string{NoStore}}};
            entries.push_back(Entry{
                definition.path.relativePath(),
                std::move(asset)});
        }

        ShellAssets shellAssets{};
        for (const auto& mapping : shellMappings) {
            const auto index = shellIndex(mapping.identifier);
            if (!index) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard shell asset identifier is invalid."));
            }
            if (shellAssets[*index]) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::Conflict,
                    "A dashboard shell asset identifier is duplicated."));
            }

            const auto entry = std::lower_bound(
                entries.begin(),
                entries.end(),
                mapping.path.relativePath(),
                [](const Entry& candidate, const std::string_view path) noexcept {
                    return std::string_view{candidate.canonicalPath} < path;
                });
            if (entry == entries.end() ||
                entry->canonicalPath != mapping.path.relativePath()) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard shell mapping references an unregistered asset."));
            }
            if (entry->asset->mimeType() != HtmlMimeType) {
                return StoreResult::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard shell mappings must reference an HTML asset."));
            }
            shellAssets[*index] = entry->asset;
        }

        return StoreResult::success(
            std::unique_ptr<DashboardStaticAssetStore>{
                new DashboardStaticAssetStore{
                    std::move(entries),
                    std::move(shellAssets),
                    aggregateBytes}});
    } catch (...) {
        return StoreResult::failure(internalError());
    }
}

const DashboardStaticAssetStore::Entry* DashboardStaticAssetStore::findEntry(
    const DashboardStaticResourcePath& path) const noexcept
{
    const auto entry = std::lower_bound(
        assets_.begin(),
        assets_.end(),
        path.relativePath(),
        [](const Entry& candidate, const std::string_view canonicalPath) noexcept {
            return std::string_view{candidate.canonicalPath} < canonicalPath;
        });
    return entry != assets_.end() &&
            entry->canonicalPath == path.relativePath()
        ? &*entry
        : nullptr;
}

Domain::Result<DashboardStaticAssetHandle>
DashboardStaticAssetStore::findStaticAsset(
    const DashboardStaticResourcePath& path) const noexcept
{
    try {
        const auto* entry = findEntry(path);
        if (entry == nullptr) {
            return Domain::Result<DashboardStaticAssetHandle>::failure(error(
                Domain::ErrorCodes::RecordNotFound,
                "The dashboard static asset was not found."));
        }
        return Domain::Result<DashboardStaticAssetHandle>::success(entry->asset);
    } catch (...) {
        return Domain::Result<DashboardStaticAssetHandle>::failure(
            internalError());
    }
}

Domain::Result<DashboardStaticAssetHandle>
DashboardStaticAssetStore::findShellAsset(
    const DashboardShellAssetId identifier) const noexcept
{
    try {
        const auto index = shellIndex(identifier);
        if (!index) {
            return Domain::Result<DashboardStaticAssetHandle>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard shell asset identifier is invalid."));
        }
        if (!shellAssets_[*index]) {
            return Domain::Result<DashboardStaticAssetHandle>::failure(error(
                Domain::ErrorCodes::RecordNotFound,
                "The dashboard shell asset was not found."));
        }
        return Domain::Result<DashboardStaticAssetHandle>::success(
            shellAssets_[*index]);
    } catch (...) {
        return Domain::Result<DashboardStaticAssetHandle>::failure(
            internalError());
    }
}

} // namespace ForgeConductor::Dashboard
