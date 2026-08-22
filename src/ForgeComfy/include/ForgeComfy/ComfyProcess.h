// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Forge::Comfy {

struct ProcessIdentity final {
    std::int32_t pid{0};
    std::string executable;
    std::string commandLine;
    std::uint64_t createTime{0};
    std::filesystem::path workingDirectory;
};

class IProcessInspector {
public:
    virtual std::optional<ProcessIdentity> snapshot(std::int32_t pid) const = 0;
    virtual bool terminate(const ProcessIdentity& identity) = 0;
    virtual ~IProcessInspector() = default;
};

class IProcessLauncher {
public:
    virtual ProcessIdentity start(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& workingDirectory) = 0;
    virtual ~IProcessLauncher() = default;
};

class Win32ProcessInspector final : public IProcessInspector {
public:
    std::optional<ProcessIdentity> snapshot(std::int32_t pid) const override;
    bool terminate(const ProcessIdentity& identity) override;
};

class Win32ProcessLauncher final : public IProcessLauncher {
public:
    ProcessIdentity start(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& workingDirectory) override;
};

} // namespace Forge::Comfy
