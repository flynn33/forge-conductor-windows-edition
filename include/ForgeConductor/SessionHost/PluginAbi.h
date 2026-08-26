#pragma once

#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/INativeSessionHostServices.h"
#include "ForgeConductor/Contracts/ISessionHostAdapter.h"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define FORGE_SESSION_HOST_ABI __stdcall
#if defined(FORGE_NATIVE_SESSION_HOST_PLUGIN_EXPORTS)
#define FORGE_SESSION_HOST_EXPORT __declspec(dllexport)
#else
#define FORGE_SESSION_HOST_EXPORT __declspec(dllimport)
#endif
#else
#define FORGE_SESSION_HOST_ABI
#define FORGE_SESSION_HOST_EXPORT
#endif

namespace ForgeConductor::SessionHost {

inline constexpr std::uint32_t NativeSessionHostPluginAbiVersion = 1U;

struct NativeSessionHostPluginDependenciesV1 final {
    std::uint32_t structureSize{sizeof(NativeSessionHostPluginDependenciesV1)};
    Contracts::INativeSessionLedger* ledger{};
    Contracts::INativeSessionTransport* transport{};
    Contracts::IContinuityDocumentCodec* codec{};
    Contracts::IUuidGenerator* uuidGenerator{};
    Contracts::IClock* clock{};
};

struct NativeSessionHostPluginManifestV1 final {
    std::uint32_t structureSize{sizeof(NativeSessionHostPluginManifestV1)};
    std::uint32_t abiVersion{NativeSessionHostPluginAbiVersion};
    const char* adapterIdentifier{};
    const char* adapterVersion{};
    std::uint32_t protocolVersion{};
    std::uint32_t capabilityBits{};
};

using CreateNativeSessionHostV1 = Contracts::ISessionHostAdapter* (
    FORGE_SESSION_HOST_ABI*)(
        const NativeSessionHostPluginDependenciesV1*) noexcept;
using DestroyNativeSessionHostV1 = void (FORGE_SESSION_HOST_ABI*)(
    Contracts::ISessionHostAdapter*) noexcept;

struct NativeSessionHostPluginApiV1 final {
    std::uint32_t structureSize{sizeof(NativeSessionHostPluginApiV1)};
    NativeSessionHostPluginManifestV1 manifest;
    CreateNativeSessionHostV1 create{};
    DestroyNativeSessionHostV1 destroy{};
};

} // namespace ForgeConductor::SessionHost

extern "C" FORGE_SESSION_HOST_EXPORT bool FORGE_SESSION_HOST_ABI
ForgeGetNativeSessionHostPluginV1(
    ForgeConductor::SessionHost::NativeSessionHostPluginApiV1* api) noexcept;

#undef FORGE_SESSION_HOST_EXPORT
#undef FORGE_SESSION_HOST_ABI
