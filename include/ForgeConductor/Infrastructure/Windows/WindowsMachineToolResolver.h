#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <string>

namespace ForgeConductor::Infrastructure::Windows {

// Resolves product toolchain executables and child search variables from
// machine-owned Windows configuration. Ambient process and per-user PATH
// values are never consulted.
class WindowsMachineToolResolver final {
public:
    [[nodiscard]] static Domain::Result<Domain::PathText>
    gitExecutable() noexcept;

    [[nodiscard]] static Domain::Result<Domain::PathText>
    powerShellExecutable() noexcept;

    [[nodiscard]] static std::string searchPath() noexcept;
    [[nodiscard]] static std::string pathExt() noexcept;
    [[nodiscard]] static std::string commandInterpreter() noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
