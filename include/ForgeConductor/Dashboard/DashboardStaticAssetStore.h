#pragma once

#include "ForgeConductor/Dashboard/IDashboardAssetStore.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ForgeConductor::Dashboard {

struct DashboardStaticAssetDefinition final {
    DashboardStaticResourcePath path;
    std::vector<std::byte> bytes;
};

// Associates a public shell route with one registered canonical HTML asset.
// Multiple shell identifiers may intentionally share one immutable asset.
struct DashboardShellAssetMapping final {
    DashboardShellAssetId identifier;
    DashboardStaticResourcePath path;
};

// Constructed once by the dashboard composition root. All validation and
// allocation happens before publication; successful instances are immutable
// and support lock-free concurrent reads.
class DashboardStaticAssetStore final : public IDashboardAssetStore {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardStaticAssetStore>>
    create(
        std::vector<DashboardStaticAssetDefinition> assets,
        std::vector<DashboardShellAssetMapping> shellMappings = {}) noexcept;

    ~DashboardStaticAssetStore() noexcept override = default;

    DashboardStaticAssetStore(const DashboardStaticAssetStore&) = delete;
    DashboardStaticAssetStore& operator=(
        const DashboardStaticAssetStore&) = delete;
    DashboardStaticAssetStore(DashboardStaticAssetStore&&) = delete;
    DashboardStaticAssetStore& operator=(DashboardStaticAssetStore&&) = delete;

    [[nodiscard]] Domain::Result<DashboardStaticAssetHandle> findStaticAsset(
        const DashboardStaticResourcePath& path) const noexcept override;

    [[nodiscard]] Domain::Result<DashboardStaticAssetHandle> findShellAsset(
        DashboardShellAssetId identifier) const noexcept override;

    [[nodiscard]] std::size_t assetCount() const noexcept
    {
        return assets_.size();
    }

    [[nodiscard]] std::size_t aggregateAssetBytes() const noexcept
    {
        return aggregateAssetBytes_;
    }

private:
    struct Entry final {
        std::string canonicalPath;
        DashboardStaticAssetHandle asset;
    };

    using ShellAssets = std::array<
        DashboardStaticAssetHandle,
        IDashboardAssetStore::MaximumShellAssetMappings>;

    DashboardStaticAssetStore(
        std::vector<Entry> assets,
        ShellAssets shellAssets,
        std::size_t aggregateAssetBytes) noexcept;

    [[nodiscard]] const Entry* findEntry(
        const DashboardStaticResourcePath& path) const noexcept;

    const std::vector<Entry> assets_;
    const ShellAssets shellAssets_;
    const std::size_t aggregateAssetBytes_{};
};

} // namespace ForgeConductor::Dashboard
