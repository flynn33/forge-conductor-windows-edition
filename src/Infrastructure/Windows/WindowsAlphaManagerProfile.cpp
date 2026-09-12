#include "ForgeConductor/Infrastructure/Windows/WindowsAlphaManagerProfile.h"

#include "Detail/UtfConversion.h"
#include "Detail/WindowsPathResolver.h"

#include <array>
#include <cstdint>
#include <format>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace WindowsDetail = ForgeConductor::Infrastructure::Windows::Detail;

Domain::Result<WindowsAlphaManagerProfile>
WindowsAlphaManagerProfile::createProfile(
    const std::string_view requestedRoot) noexcept
{
    try {
        auto resolved = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
            requestedRoot);
        if (!resolved) {
            return Domain::Result<WindowsAlphaManagerProfile>::failure(
                std::move(resolved).error());
        }
        auto path = WindowsDetail::WindowsPathResolver::toPathText(resolved.value());
        if (!path) {
            return Domain::Result<WindowsAlphaManagerProfile>::failure(
                std::move(path).error());
        }

        // This identifier is a collision-resistant naming scope, not a secret
        // or authentication primitive. FNV-1a keeps both processes independent
        // of mutable storage while producing the same bounded Windows names.
        std::uint64_t identifier = 14695981039346656037ULL;
        for (const unsigned char byte : path.value().value()) {
            identifier ^= static_cast<std::uint64_t>(byte);
            identifier *= 1099511628211ULL;
        }
        const std::string token = std::format("{:016x}", identifier);
        std::string purposeSuffix = "alpha-" + token;
        std::wstring registrySubkey =
            L"Software\\Forge Conductor\\AlphaProfiles\\";
        registrySubkey.append(token.begin(), token.end());
        registrySubkey.append(L"\\SecureStorage");

        return Domain::Result<WindowsAlphaManagerProfile>::success(
            WindowsAlphaManagerProfile{
                std::move(path).value(), std::move(resolved).value(),
                std::move(purposeSuffix), std::move(registrySubkey)});
    } catch (...) {
        return Domain::Result<WindowsAlphaManagerProfile>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The isolated Alpha Manager profile could not be prepared."));
    }
}

WindowsAlphaManagerProfile::WindowsAlphaManagerProfile(
    Domain::PathText dataRoot,
    std::wstring nativeDataRoot,
    std::string purposeSuffix,
    std::wstring secureStorageRegistrySubkey) noexcept
    : dataRoot_{std::move(dataRoot)},
      nativeDataRoot_{std::move(nativeDataRoot)},
      purposeSuffix_{std::move(purposeSuffix)},
      secureStorageRegistrySubkey_{std::move(secureStorageRegistrySubkey)}
{
}

Domain::Result<WindowsAlphaManagerProfile>
WindowsAlphaManagerProfile::create(
    const Domain::PathText& requestedRoot) noexcept
{
    return createProfile(requestedRoot.value());
}

Domain::Result<WindowsAlphaManagerProfile>
WindowsAlphaManagerProfile::create(
    const std::wstring_view requestedRoot) noexcept
{
    auto converted = WindowsDetail::strictUtf16ToUtf8(requestedRoot);
    if (!converted) {
        return Domain::Result<WindowsAlphaManagerProfile>::failure(
            std::move(converted).error());
    }
    return createProfile(converted.value());
}

} // namespace ForgeConductor::Infrastructure::Windows
