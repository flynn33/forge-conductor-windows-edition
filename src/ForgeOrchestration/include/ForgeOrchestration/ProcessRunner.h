// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Forge::Orchestration {

struct ProcessResult final {
    int exitCode{0};
    std::string stdoutText;
    std::string stderrText;
    bool timedOut{false};
};

class ProcessRunner final {
public:
    ProcessResult run(
        const std::string& command,
        const std::optional<std::string>& workingDirectory,
        int timeoutSec,
        std::size_t maxOutputBytes = 256 * 1024) const;
};

} // namespace Forge::Orchestration
