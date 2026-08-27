#include "ForgeConductor/Dashboard/DashboardStaticAssetStore.h"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

using Asset = Dashboard::DashboardStaticAsset;
using AssetDefinition = Dashboard::DashboardStaticAssetDefinition;
using AssetHandle = Dashboard::DashboardStaticAssetHandle;
using AssetStore = Dashboard::DashboardStaticAssetStore;
using AssetStoreInterface = Dashboard::IDashboardAssetStore;
using ShellId = Dashboard::DashboardShellAssetId;
using ShellMapping = Dashboard::DashboardShellAssetMapping;
using StaticPath = Dashboard::DashboardStaticResourcePath;

static_assert(std::is_abstract_v<AssetStoreInterface>);
static_assert(std::is_final_v<AssetStore>);
static_assert(std::is_final_v<Asset>);
static_assert(!std::is_copy_constructible_v<Asset>);
static_assert(!std::is_move_constructible_v<Asset>);
static_assert(std::is_same_v<
              decltype(std::declval<const Asset&>().bytes()),
              std::span<const std::byte>>);
static_assert(std::is_same_v<
              AssetHandle,
              std::shared_ptr<const Dashboard::DashboardStaticAsset>>);
static_assert(noexcept(AssetStore::create({})));
static_assert(noexcept(
    std::declval<const AssetStore&>().findStaticAsset(
        std::declval<const StaticPath&>())));
static_assert(noexcept(
    std::declval<const AssetStore&>().findShellAsset(ShellId::Root)));
static_assert(AssetStoreInterface::MaximumAssetBodyBytes == 2'080'768U);
static_assert(
    AssetStoreInterface::MaximumAggregateAssetBytes == 8U * 1024U * 1024U);
static_assert(AssetStoreInterface::MaximumAssetCount == 128U);
static_assert(AssetStoreInterface::MaximumShellAssetMappings == 4U);

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

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, "failure used the wrong code");
    require(!result.error().retryable, "asset-store failure was retryable");
}

[[nodiscard]] StaticPath path(const std::string_view rawTarget)
{
    return take(StaticPath::decode(rawTarget));
}

[[nodiscard]] std::vector<std::byte> textBytes(const std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] AssetDefinition definition(
    const std::string_view rawTarget,
    const std::size_t byteCount = 1U,
    const std::byte value = std::byte{0x5a})
{
    return AssetDefinition{
        path(rawTarget),
        std::vector<std::byte>(byteCount, value)};
}

[[nodiscard]] std::unique_ptr<AssetStore> createStore(
    std::vector<AssetDefinition> definitions,
    std::vector<ShellMapping> mappings = {})
{
    return take(AssetStore::create(
        std::move(definitions),
        std::move(mappings)));
}

void exactPerAssetBoundIsEnforced()
{
    const auto maximum = AssetStoreInterface::MaximumAssetBodyBytes;
    std::vector<AssetDefinition> definitions;
    definitions.push_back(definition("/static/maximum.js", maximum));
    auto store = createStore(std::move(definitions));

    require(store->assetCount() == 1U, "exact-bound store count was incorrect");
    require(
        store->aggregateAssetBytes() == maximum,
        "exact-bound aggregate byte count was incorrect");
    const auto asset = take(store->findStaticAsset(path("/static/maximum.js")));
    require(asset->bytes().size() == maximum, "exact asset bound was not retained");
    require(
        asset->mimeType() == "application/javascript; charset=utf-8",
        "JavaScript MIME metadata was incorrect");
    require(asset->cacheControl() == "no-store", "cache metadata was incorrect");

    store.reset();
    std::vector<AssetDefinition> oversized;
    oversized.push_back(definition("/static/oversized.js", maximum + 1U));
    const auto rejected = AssetStore::create(std::move(oversized));
    requireError(
        rejected,
        Domain::ErrorCodes::PayloadTooLarge,
        "asset body ceiling plus one was accepted");
}

[[nodiscard]] std::vector<AssetDefinition> aggregateDefinitions(
    const std::size_t finalExtraBytes)
{
    const auto perAsset = AssetStoreInterface::MaximumAssetBodyBytes;
    const auto aggregate = AssetStoreInterface::MaximumAggregateAssetBytes;
    const auto finalBytes = aggregate - (perAsset * 4U) + finalExtraBytes;

    std::vector<AssetDefinition> definitions;
    definitions.reserve(5U);
    definitions.push_back(definition("/static/bound0.js", perAsset));
    definitions.push_back(definition("/static/bound1.js", perAsset));
    definitions.push_back(definition("/static/bound2.js", perAsset));
    definitions.push_back(definition("/static/bound3.js", perAsset));
    definitions.push_back(definition("/static/bound4.js", finalBytes));
    return definitions;
}

void exactAggregateBoundIsEnforced()
{
    constexpr auto Aggregate =
        AssetStoreInterface::MaximumAggregateAssetBytes;
    constexpr auto PerAsset = AssetStoreInterface::MaximumAssetBodyBytes;
    static_assert(Aggregate > PerAsset * 4U);
    static_assert(Aggregate - (PerAsset * 4U) <= PerAsset);

    auto exact = createStore(aggregateDefinitions(0U));
    require(exact->assetCount() == 5U, "exact aggregate lost an asset");
    require(
        exact->aggregateAssetBytes() == Aggregate,
        "exact aggregate byte ceiling was not accepted");
    exact.reset();

    const auto rejected = AssetStore::create(aggregateDefinitions(1U));
    requireError(
        rejected,
        Domain::ErrorCodes::LimitExceeded,
        "aggregate byte ceiling plus one was accepted");
}

void canonicalDuplicatesAndInvalidPathsAreRejected()
{
    const auto direct = path("/static/app.js");
    const auto encoded = path("/static/%61pp.js");
    require(
        direct.relativePath() == encoded.relativePath(),
        "canonical percent decoding did not converge");

    std::vector<AssetDefinition> duplicates;
    duplicates.push_back(AssetDefinition{direct, textBytes("first")});
    duplicates.push_back(AssetDefinition{encoded, textBytes("second")});
    const auto duplicateResult = AssetStore::create(std::move(duplicates));
    requireError(
        duplicateResult,
        Domain::ErrorCodes::Conflict,
        "duplicate canonical static paths were accepted");

    const auto uppercaseName = StaticPath::decode("/static/App.js");
    require(!uppercaseName, "uppercase static path was canonicalized implicitly");
    require(
        uppercaseName.error().code == Domain::ErrorCodes::PathOutsideAuthority,
        "uppercase path used the wrong error code");
    const auto uppercaseExtension = StaticPath::decode("/static/app.JS");
    require(!uppercaseExtension, "uppercase extension was accepted");
    const auto dotSegment = StaticPath::decode("/static/ui/../app.js");
    require(!dotSegment, "dot-dot static path was accepted");
    const auto encodedSeparator = StaticPath::decode("/static/ui%2fapp.js");
    require(!encodedSeparator, "encoded static separator was accepted");
}

void emptyAndCountInvalidConstructionFails()
{
    const auto emptyStore = AssetStore::create({});
    requireError(
        emptyStore,
        Domain::ErrorCodes::InvalidRequest,
        "empty asset store was accepted");

    std::vector<AssetDefinition> emptyAsset;
    emptyAsset.push_back(AssetDefinition{
        path("/static/empty.js"),
        {}});
    const auto emptyAssetResult = AssetStore::create(std::move(emptyAsset));
    requireError(
        emptyAssetResult,
        Domain::ErrorCodes::InvalidRequest,
        "empty static asset was accepted");

    std::vector<AssetDefinition> exactCount;
    exactCount.reserve(AssetStoreInterface::MaximumAssetCount);
    for (std::size_t index{};
         index < AssetStoreInterface::MaximumAssetCount;
         ++index) {
        exactCount.push_back(definition(
            "/static/exact" + std::to_string(index) + ".js"));
    }
    auto exactCountStore = createStore(std::move(exactCount));
    require(
        exactCountStore->assetCount() == AssetStoreInterface::MaximumAssetCount,
        "exact asset count ceiling was not accepted");
    require(
        exactCountStore->aggregateAssetBytes() ==
            AssetStoreInterface::MaximumAssetCount,
        "exact asset count changed aggregate bytes");
    exactCountStore.reset();

    std::vector<AssetDefinition> tooMany;
    tooMany.reserve(AssetStoreInterface::MaximumAssetCount + 1U);
    for (std::size_t index{};
         index < AssetStoreInterface::MaximumAssetCount + 1U;
         ++index) {
        tooMany.push_back(definition(
            "/static/asset" + std::to_string(index) + ".js"));
    }
    const auto tooManyResult = AssetStore::create(std::move(tooMany));
    requireError(
        tooManyResult,
        Domain::ErrorCodes::LimitExceeded,
        "asset count ceiling plus one was accepted");
}

void shellRoutesShareTheRegisteredHtmlAsset()
{
    const auto indexPath = path("/static/index.html");
    std::vector<AssetDefinition> definitions;
    definitions.push_back(AssetDefinition{
        indexPath,
        textBytes("<!doctype html><title>test</title>")});
    std::vector<ShellMapping> mappings{
        {ShellId::Root, indexPath},
        {ShellId::Index, indexPath},
        {ShellId::Control, indexPath},
        {ShellId::Manager, indexPath},
    };
    auto store = createStore(std::move(definitions), std::move(mappings));
    AssetStoreInterface& interfaceStore = *store;

    const auto staticAsset = take(interfaceStore.findStaticAsset(indexPath));
    const std::vector<ShellId> identifiers{
        ShellId::Root,
        ShellId::Index,
        ShellId::Control,
        ShellId::Manager,
    };
    for (const auto identifier : identifiers) {
        const auto shellAsset = take(interfaceStore.findShellAsset(identifier));
        require(shellAsset == staticAsset, "shell mapping copied or changed its asset");
        require(
            shellAsset->mimeType() == "text/html; charset=utf-8",
            "shell mapping did not retain HTML MIME metadata");
        require(
            shellAsset->cacheControl() == "no-store",
            "shell mapping did not retain cache metadata");
    }
}

void invalidShellMappingsFailConstruction()
{
    const auto indexPath = path("/static/index.html");
    const auto scriptPath = path("/static/app.js");

    std::vector<AssetDefinition> duplicateDefinitions;
    duplicateDefinitions.push_back(AssetDefinition{
        indexPath,
        textBytes("html")});
    std::vector<ShellMapping> duplicateMappings{
        {ShellId::Root, indexPath},
        {ShellId::Root, indexPath},
    };
    const auto duplicate = AssetStore::create(
        std::move(duplicateDefinitions),
        std::move(duplicateMappings));
    requireError(
        duplicate,
        Domain::ErrorCodes::Conflict,
        "duplicate shell identifier was accepted");

    std::vector<AssetDefinition> missingDefinitions;
    missingDefinitions.push_back(AssetDefinition{
        indexPath,
        textBytes("html")});
    std::vector<ShellMapping> missingMappings{
        {ShellId::Root, path("/static/missing.html")},
    };
    const auto missing = AssetStore::create(
        std::move(missingDefinitions),
        std::move(missingMappings));
    requireError(
        missing,
        Domain::ErrorCodes::InvalidRequest,
        "unregistered shell mapping target was accepted");

    std::vector<AssetDefinition> nonHtmlDefinitions;
    nonHtmlDefinitions.push_back(AssetDefinition{
        scriptPath,
        textBytes("script")});
    std::vector<ShellMapping> nonHtmlMappings{
        {ShellId::Root, scriptPath},
    };
    const auto nonHtml = AssetStore::create(
        std::move(nonHtmlDefinitions),
        std::move(nonHtmlMappings));
    requireError(
        nonHtml,
        Domain::ErrorCodes::InvalidRequest,
        "non-HTML shell mapping was accepted");

    std::vector<AssetDefinition> invalidIdDefinitions;
    invalidIdDefinitions.push_back(AssetDefinition{
        indexPath,
        textBytes("html")});
    std::vector<ShellMapping> invalidIdMappings{
        {static_cast<ShellId>(255U), indexPath},
    };
    const auto invalidId = AssetStore::create(
        std::move(invalidIdDefinitions),
        std::move(invalidIdMappings));
    requireError(
        invalidId,
        Domain::ErrorCodes::InvalidRequest,
        "invalid shell identifier was accepted");

    std::vector<AssetDefinition> tooManyDefinitions;
    tooManyDefinitions.push_back(AssetDefinition{
        indexPath,
        textBytes("html")});
    std::vector<ShellMapping> tooManyMappings{
        {ShellId::Root, indexPath},
        {ShellId::Index, indexPath},
        {ShellId::Control, indexPath},
        {ShellId::Manager, indexPath},
        {ShellId::Root, indexPath},
    };
    const auto tooMany = AssetStore::create(
        std::move(tooManyDefinitions),
        std::move(tooManyMappings));
    requireError(
        tooMany,
        Domain::ErrorCodes::LimitExceeded,
        "shell mapping count ceiling plus one was accepted");
}

void lookupOwnsBytesAndReportsMissingEntries()
{
    const auto scriptPath = path("/static/app.js");
    auto callerBytes = textBytes("owned");
    AssetDefinition callerDefinition{scriptPath, callerBytes};
    std::vector<AssetDefinition> definitions{callerDefinition};
    auto store = createStore(std::move(definitions));

    callerBytes[0] = std::byte{0x00};
    callerDefinition.bytes[1] = std::byte{0x00};
    auto retained = take(store->findStaticAsset(scriptPath));
    require(
        retained->bytes()[0] == static_cast<std::byte>('o'),
        "asset borrowed the caller byte buffer");
    require(
        retained->bytes()[1] == static_cast<std::byte>('w'),
        "asset borrowed the registration byte buffer");

    const auto missing = store->findStaticAsset(path("/static/missing.js"));
    requireError(
        missing,
        Domain::ErrorCodes::RecordNotFound,
        "missing static asset returned success");
    const auto missingShell = store->findShellAsset(ShellId::Root);
    requireError(
        missingShell,
        Domain::ErrorCodes::RecordNotFound,
        "unmapped shell asset returned success");
    const auto invalidShell = store->findShellAsset(
        static_cast<ShellId>(255U));
    requireError(
        invalidShell,
        Domain::ErrorCodes::InvalidRequest,
        "invalid shell lookup returned success");

    store.reset();
    require(retained->bytes().size() == 5U, "retained asset lost its bytes");
    require(
        retained->mimeType() == "application/javascript; charset=utf-8",
        "retained asset lost its MIME metadata");
    require(
        retained->cacheControl() == "no-store",
        "retained asset lost its cache metadata");
}

void concurrentReadsAreStable()
{
    constexpr std::size_t ThreadCount = 16U;
    constexpr std::size_t Iterations = 5'000U;
    const auto indexPath = path("/static/index.html");
    const auto scriptPath = path("/static/app.js");
    std::vector<AssetDefinition> definitions;
    definitions.push_back(AssetDefinition{indexPath, textBytes("html")});
    definitions.push_back(AssetDefinition{scriptPath, textBytes("script")});
    std::vector<ShellMapping> mappings{
        {ShellId::Root, indexPath},
        {ShellId::Index, indexPath},
        {ShellId::Control, indexPath},
        {ShellId::Manager, indexPath},
    };
    auto store = createStore(std::move(definitions), std::move(mappings));
    const auto expectedScript = take(store->findStaticAsset(scriptPath));
    const auto expectedShell = take(store->findShellAsset(ShellId::Root));

    std::atomic_bool failure{};
    std::atomic<std::size_t> observations{};
    std::vector<std::jthread> workers;
    workers.reserve(ThreadCount);
    for (std::size_t threadIndex{}; threadIndex < ThreadCount; ++threadIndex) {
        workers.emplace_back([&, threadIndex](std::stop_token) {
            constexpr ShellId Identifiers[]{
                ShellId::Root,
                ShellId::Index,
                ShellId::Control,
                ShellId::Manager,
            };
            for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
                const auto staticResult = store->findStaticAsset(scriptPath);
                const auto shellResult = store->findShellAsset(
                    Identifiers[(iteration + threadIndex) % 4U]);
                if (!staticResult || !shellResult ||
                    staticResult.value() != expectedScript ||
                    shellResult.value() != expectedShell ||
                    staticResult.value()->bytes().size() != 6U ||
                    shellResult.value()->bytes().size() != 4U) {
                    failure.store(true, std::memory_order_release);
                }
                observations.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    workers.clear();

    require(!failure.load(), "concurrent lookup observed mutable state");
    require(
        observations.load() == ThreadCount * Iterations,
        "concurrent lookup lost an observation");
    require(store->assetCount() == 2U, "concurrent lookup changed asset count");
    require(
        store->aggregateAssetBytes() == 10U,
        "concurrent lookup changed aggregate bytes");
}

} // namespace

int main()
{
    try {
        exactPerAssetBoundIsEnforced();
        exactAggregateBoundIsEnforced();
        canonicalDuplicatesAndInvalidPathsAreRejected();
        emptyAndCountInvalidConstructionFails();
        shellRoutesShareTheRegisteredHtmlAsset();
        invalidShellMappingsFailConstruction();
        lookupOwnsBytesAndReportsMissingEntries();
        concurrentReadsAreStable();
        std::cout << "Dashboard static asset store tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Dashboard static asset store tests failed after "
                  << assertionCount << " assertions: "
                  << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard static asset store tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
