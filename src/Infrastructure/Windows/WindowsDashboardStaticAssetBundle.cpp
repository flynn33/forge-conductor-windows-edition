#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.h"

#include "../../Hosts/Manager/Resources/Dashboard/DashboardResourceIds.h"

#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"
#include "ForgeConductor/Domain/Error.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

struct EmbeddedAsset final {
    std::uint16_t resourceIdentifier;
    std::string_view canonicalPath;
};

constexpr std::array<EmbeddedAsset, 6U> EmbeddedAssets{{
    {FORGE_DASHBOARD_INDEX_HTML_RESOURCE_ID, "/static/index.html"},
    {FORGE_DASHBOARD_CONTROL_HTML_RESOURCE_ID, "/static/control.html"},
    {FORGE_DASHBOARD_CSS_RESOURCE_ID, "/static/dashboard.css"},
    {FORGE_DASHBOARD_AUTH_JS_RESOURCE_ID, "/static/auth.js"},
    {FORGE_DASHBOARD_TELEMETRY_JS_RESOURCE_ID, "/static/telemetry.js"},
    {FORGE_DASHBOARD_CONTROL_JS_RESOURCE_ID, "/static/control.js"},
}};

[[nodiscard]] Domain::Error error(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] Domain::Error resourceError(const EmbeddedAsset& asset)
{
    return error(
        Domain::ErrorCodes::IntegrityFailure,
        "The packaged dashboard resource is missing or unreadable: " +
            std::string{asset.canonicalPath});
}

[[nodiscard]] Domain::Result<std::vector<std::byte>> loadResource(
    const HMODULE module,
    const EmbeddedAsset& asset) noexcept
{
    try {
        const auto resource = ::FindResourceW(
            module,
            MAKEINTRESOURCEW(asset.resourceIdentifier),
            MAKEINTRESOURCEW(10U));
        if (resource == nullptr) {
            return Domain::Result<std::vector<std::byte>>::failure(
                resourceError(asset));
        }

        const DWORD size = ::SizeofResource(module, resource);
        if (size == 0U) {
            return Domain::Result<std::vector<std::byte>>::failure(
                resourceError(asset));
        }
        if (size > Dashboard::IDashboardAssetStore::MaximumAssetBodyBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(error(
                Domain::ErrorCodes::PayloadTooLarge,
                "The packaged dashboard resource exceeds the static asset limit: " +
                    std::string{asset.canonicalPath}));
        }

        const auto loaded = ::LoadResource(module, resource);
        if (loaded == nullptr) {
            return Domain::Result<std::vector<std::byte>>::failure(
                resourceError(asset));
        }
        const auto* bytes = static_cast<const std::byte*>(
            ::LockResource(loaded));
        if (bytes == nullptr) {
            return Domain::Result<std::vector<std::byte>>::failure(
                resourceError(asset));
        }

        return Domain::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>{bytes, bytes + size});
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(error(
            Domain::ErrorCodes::InternalFailure,
            "The packaged dashboard resource could not be loaded."));
    }
}

[[nodiscard]] Domain::Result<Dashboard::DashboardStaticResourcePath>
decodePath(const std::string_view path) noexcept
{
    auto decoded = Dashboard::DashboardStaticResourcePath::decode(path);
    if (!decoded) {
        return Domain::Result<Dashboard::DashboardStaticResourcePath>::failure(
            error(
                Domain::ErrorCodes::IntegrityFailure,
                "A packaged dashboard resource path is invalid."));
    }
    return decoded;
}

} // namespace

Domain::Result<std::unique_ptr<Dashboard::DashboardStaticAssetStore>>
WindowsDashboardStaticAssetBundle::create() noexcept
{
    using BundleResult = Domain::Result<
        std::unique_ptr<Dashboard::DashboardStaticAssetStore>>;
    try {
        const HMODULE module = ::GetModuleHandleW(nullptr);
        if (module == nullptr) {
            return BundleResult::failure(error(
                Domain::ErrorCodes::InternalFailure,
                "The current dashboard host module is unavailable."));
        }

        std::vector<Dashboard::DashboardStaticAssetDefinition> definitions;
        definitions.reserve(EmbeddedAssets.size());
        for (const auto& embedded : EmbeddedAssets) {
            auto path = decodePath(embedded.canonicalPath);
            if (!path) {
                return BundleResult::failure(std::move(path).error());
            }
            auto bytes = loadResource(module, embedded);
            if (!bytes) {
                return BundleResult::failure(std::move(bytes).error());
            }
            definitions.push_back(Dashboard::DashboardStaticAssetDefinition{
                std::move(path).value(),
                std::move(bytes).value()});
        }

        auto indexPath = decodePath("/static/index.html");
        auto controlPath = decodePath("/static/control.html");
        if (!indexPath || !controlPath) {
            return BundleResult::failure(error(
                Domain::ErrorCodes::IntegrityFailure,
                "A packaged dashboard shell path is invalid."));
        }

        std::vector<Dashboard::DashboardShellAssetMapping> mappings;
        mappings.reserve(
            Dashboard::IDashboardAssetStore::MaximumShellAssetMappings);
        mappings.push_back({
            Dashboard::DashboardShellAssetId::Root,
            indexPath.value()});
        mappings.push_back({
            Dashboard::DashboardShellAssetId::Index,
            indexPath.value()});
        mappings.push_back({
            Dashboard::DashboardShellAssetId::Control,
            controlPath.value()});
        mappings.push_back({
            Dashboard::DashboardShellAssetId::Manager,
            controlPath.value()});

        return Dashboard::DashboardStaticAssetStore::create(
            std::move(definitions), std::move(mappings));
    } catch (...) {
        return BundleResult::failure(error(
            Domain::ErrorCodes::InternalFailure,
            "The packaged dashboard asset bundle failed safely."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
