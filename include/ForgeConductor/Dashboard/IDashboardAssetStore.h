#pragma once

#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {

class DashboardStaticAssetStore;

enum class DashboardShellAssetId : std::uint8_t {
    Root,
    Index,
    Control,
    Manager,
};

// Immutable application-owned representation. Handles returned by a store
// retain these bytes independently of both registration buffers and store
// lifetime.
class DashboardStaticAsset final {
public:
    ~DashboardStaticAsset() noexcept = default;

    DashboardStaticAsset(const DashboardStaticAsset&) = delete;
    DashboardStaticAsset& operator=(const DashboardStaticAsset&) = delete;
    DashboardStaticAsset(DashboardStaticAsset&&) = delete;
    DashboardStaticAsset& operator=(DashboardStaticAsset&&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return bytes_;
    }

    [[nodiscard]] const std::string& mimeType() const noexcept
    {
        return mimeType_;
    }

    [[nodiscard]] const std::string& cacheControl() const noexcept
    {
        return cacheControl_;
    }

private:
    friend class DashboardStaticAssetStore;

    DashboardStaticAsset(
        std::vector<std::byte> bytes,
        std::string mimeType,
        std::string cacheControl) noexcept
        : bytes_{std::move(bytes)},
          mimeType_{std::move(mimeType)},
          cacheControl_{std::move(cacheControl)}
    {
    }

    const std::vector<std::byte> bytes_;
    const std::string mimeType_;
    const std::string cacheControl_;
};

using DashboardStaticAssetHandle =
    std::shared_ptr<const DashboardStaticAsset>;

// Platform-neutral application boundary. Implementations perform no loading
// during lookup and return retained immutable values rather than borrowed
// caller buffers.
class IDashboardAssetStore {
public:
    static constexpr std::size_t MaximumAssetBodyBytes = 2'080'768U;
    static constexpr std::size_t MaximumAggregateAssetBytes =
        8U * 1024U * 1024U;
    static constexpr std::size_t MaximumAssetCount = 128U;
    static constexpr std::size_t MaximumShellAssetMappings = 4U;

    virtual ~IDashboardAssetStore() = default;

    [[nodiscard]] virtual Domain::Result<DashboardStaticAssetHandle>
    findStaticAsset(const DashboardStaticResourcePath& path) const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<DashboardStaticAssetHandle>
    findShellAsset(DashboardShellAssetId identifier) const noexcept = 0;
};

} // namespace ForgeConductor::Dashboard
