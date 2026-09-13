#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

// An isolated profile. Its stable scope separates the Manager lease, named
// pipe, and DPAPI registry entries from production and every other selected
// root. The historical type name and --alpha-root option remain source/CLI
// compatible; ordinary startup resolves the production profile under LocalAppData.
class WindowsAlphaManagerProfile final {
public:
    [[nodiscard]] static Domain::Result<std::wstring>
    persistentDataRoot() noexcept;
    [[nodiscard]] static Domain::Result<WindowsAlphaManagerProfile> create(
        const Domain::PathText& requestedRoot) noexcept;
    [[nodiscard]] static Domain::Result<WindowsAlphaManagerProfile> create(
        std::wstring_view requestedRoot) noexcept;

    [[nodiscard]] const Domain::PathText& dataRoot() const noexcept
    {
        return dataRoot_;
    }

    [[nodiscard]] std::wstring_view nativeDataRoot() const noexcept
    {
        return nativeDataRoot_;
    }

    [[nodiscard]] std::string_view purposeSuffix() const noexcept
    {
        return purposeSuffix_;
    }

    [[nodiscard]] std::wstring_view secureStorageRegistrySubkey() const noexcept
    {
        return secureStorageRegistrySubkey_;
    }

private:
    [[nodiscard]] static Domain::Result<WindowsAlphaManagerProfile>
    createProfile(std::string_view requestedRoot) noexcept;

    WindowsAlphaManagerProfile(
        Domain::PathText dataRoot,
        std::wstring nativeDataRoot,
        std::string purposeSuffix,
        std::wstring secureStorageRegistrySubkey) noexcept;

    Domain::PathText dataRoot_;
    std::wstring nativeDataRoot_;
    std::string purposeSuffix_;
    std::wstring secureStorageRegistrySubkey_;
};

} // namespace ForgeConductor::Infrastructure::Windows
