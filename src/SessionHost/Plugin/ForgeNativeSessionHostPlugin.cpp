#define FORGE_NATIVE_SESSION_HOST_PLUGIN_EXPORTS
#include "ForgeConductor/SessionHost/PluginAbi.h"

#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"

#include <new>
#include <utility>

namespace ForgeConductor::SessionHost {
namespace {

constexpr std::uint32_t CapabilityCreate = 1U << 0U;
constexpr std::uint32_t CapabilityBootstrap = 1U << 1U;
constexpr std::uint32_t CapabilityUsage = 1U << 2U;
constexpr std::uint32_t CapabilityResume = 1U << 3U;
constexpr std::uint32_t CapabilityIdempotency = 1U << 4U;
constexpr std::uint32_t CapabilityQueryByKey = 1U << 5U;
constexpr std::uint32_t CapabilityRecovery = 1U << 6U;
constexpr std::uint32_t CapabilityCancellation = 1U << 7U;

[[nodiscard]] Contracts::ISessionHostAdapter* __stdcall createAdapter(
    const NativeSessionHostPluginDependenciesV1* dependencies) noexcept
{
    try {
        if (dependencies == nullptr ||
            dependencies->structureSize !=
                sizeof(NativeSessionHostPluginDependenciesV1) ||
            dependencies->ledger == nullptr ||
            dependencies->transport == nullptr ||
            dependencies->codec == nullptr ||
            dependencies->uuidGenerator == nullptr ||
            dependencies->clock == nullptr) {
            return nullptr;
        }
        auto adapterId = Domain::AdapterId::parse(
            ForgeNativeSessionHostAdapter::AdapterIdentifier);
        if (!adapterId) {
            return nullptr;
        }
        return new (std::nothrow) ForgeNativeSessionHostAdapter{
            std::move(adapterId).value(),
            *dependencies->ledger,
            *dependencies->transport,
            *dependencies->codec,
            *dependencies->uuidGenerator,
            *dependencies->clock};
    } catch (...) {
        return nullptr;
    }
}

void __stdcall destroyAdapter(
    Contracts::ISessionHostAdapter* adapter) noexcept
{
    try {
        delete adapter;
    } catch (...) {
    }
}

} // namespace
} // namespace ForgeConductor::SessionHost

extern "C" __declspec(dllexport) bool __stdcall
ForgeGetNativeSessionHostPluginV1(
    ForgeConductor::SessionHost::NativeSessionHostPluginApiV1* api) noexcept
{
    using namespace ForgeConductor::SessionHost;
    try {
        if (api == nullptr ||
            api->structureSize != sizeof(NativeSessionHostPluginApiV1)) {
            return false;
        }
        api->manifest = NativeSessionHostPluginManifestV1{
            sizeof(NativeSessionHostPluginManifestV1),
            NativeSessionHostPluginAbiVersion,
            ForgeNativeSessionHostAdapter::AdapterIdentifier.data(),
            ForgeNativeSessionHostAdapter::AdapterVersion.data(),
            ForgeNativeSessionHostAdapter::ProtocolVersion,
            CapabilityCreate | CapabilityBootstrap | CapabilityUsage |
                CapabilityResume | CapabilityIdempotency |
                CapabilityQueryByKey | CapabilityRecovery |
                CapabilityCancellation};
        api->create = &createAdapter;
        api->destroy = &destroyAdapter;
        return true;
    } catch (...) {
        return false;
    }
}
