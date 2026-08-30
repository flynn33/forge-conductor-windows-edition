#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.h"

#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Bundle = Windows::WindowsDashboardStaticAssetBundle;
using ShellId = Dashboard::DashboardShellAssetId;
using StaticPath = Dashboard::DashboardStaticResourcePath;

static_assert(std::is_final_v<Bundle>);
static_assert(!std::is_constructible_v<Bundle>);
static_assert(noexcept(Bundle::create()));
static_assert(std::is_same_v<
              decltype(Bundle::create()),
              Domain::Result<
                  std::unique_ptr<Dashboard::DashboardStaticAssetStore>>>);

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

[[nodiscard]] StaticPath path(const std::string_view rawTarget)
{
    return take(StaticPath::decode(rawTarget));
}

[[nodiscard]] std::string text(
    const Dashboard::DashboardStaticAssetHandle& asset)
{
    require(asset != nullptr, "asset handle was null");
    std::string value;
    value.reserve(asset->bytes().size());
    for (const auto byte : asset->bytes()) {
        value.push_back(static_cast<char>(byte));
    }
    return value;
}

void loadsExactPackagedAssetSet()
{
    auto store = take(Bundle::create());
    require(store->assetCount() == 6U, "packaged asset count was not six");
    require(
        store->aggregateAssetBytes() > 0U,
        "packaged aggregate byte count was zero");
    require(
        store->aggregateAssetBytes() <=
            Dashboard::IDashboardAssetStore::MaximumAggregateAssetBytes,
        "packaged aggregate exceeded the store ceiling");

    struct ExpectedAsset final {
        std::string_view path;
        std::string_view mimeType;
        std::string_view requiredText;
    };
    constexpr std::array<ExpectedAsset, 6U> ExpectedAssets{{
        {"/static/index.html", "text/html; charset=utf-8", "panel-sys-strip"},
        {"/static/control.html", "text/html; charset=utf-8", "manager-settings-form"},
        {"/static/dashboard.css", "text/css; charset=utf-8", "@media (max-width: 52rem)"},
        {"/static/auth.js", "application/javascript; charset=utf-8", "DashboardClient"},
        {"/static/telemetry.js", "application/javascript; charset=utf-8", "/api/stream?hz=2"},
        {"/static/control.js", "application/javascript; charset=utf-8", "/api/manager/settings"},
    }};

    for (const auto& expected : ExpectedAssets) {
        const auto asset = take(store->findStaticAsset(path(expected.path)));
        require(!asset->bytes().empty(), "packaged asset was empty");
        require(
            asset->bytes().size() <=
                Dashboard::IDashboardAssetStore::MaximumAssetBodyBytes,
            "packaged asset exceeded the per-asset ceiling");
        require(
            asset->mimeType() == expected.mimeType,
            "packaged asset MIME type was incorrect");
        require(
            asset->cacheControl() == "no-store",
            "packaged asset cache policy was incorrect");
        const auto contents = text(asset);
        require(
            contents.find(expected.requiredText) != std::string::npos,
            "packaged asset did not contain its identifying contract text");
        require(
            !(contents.size() >= 3U &&
              static_cast<unsigned char>(contents[0]) == 0xefU &&
              static_cast<unsigned char>(contents[1]) == 0xbbU &&
              static_cast<unsigned char>(contents[2]) == 0xbfU),
            "packaged asset contained a UTF-8 byte-order mark");
    }
}

void mapsAllShellAliasesToRetainedHtml()
{
    auto store = take(Bundle::create());
    const auto root = take(store->findShellAsset(ShellId::Root));
    const auto index = take(store->findShellAsset(ShellId::Index));
    const auto control = take(store->findShellAsset(ShellId::Control));
    const auto manager = take(store->findShellAsset(ShellId::Manager));

    require(root.get() == index.get(), "root and index did not share one asset");
    require(
        control.get() == manager.get(),
        "control and manager did not share one asset");
    require(root.get() != control.get(), "telemetry and control shells aliased");
    require(
        root->mimeType() == "text/html; charset=utf-8",
        "root shell was not HTML");
    require(
        control->mimeType() == "text/html; charset=utf-8",
        "control shell was not HTML");

    store.reset();
    require(
        text(root).find("Forge Conductor Dashboard") != std::string::npos,
        "root handle did not retain bytes after store destruction");
    require(
        text(control).find("Forge Conductor Manager and Operations") !=
            std::string::npos,
        "control handle did not retain bytes after store destruction");
}

} // namespace

int main()
{
    try {
        loadsExactPackagedAssetSet();
        mapsAllShellAliasesToRetainedHtml();
        std::cout << "Windows dashboard static asset bundle tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows dashboard static asset bundle tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
