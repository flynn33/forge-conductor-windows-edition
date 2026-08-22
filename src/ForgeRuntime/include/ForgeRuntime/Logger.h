// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

namespace Forge::Runtime {

enum class LogLevel { Debug, Info, Warn, Error };

class IForgeLogger {
public:
    virtual void log(
        LogLevel level,
        const std::string& event,
        const std::map<std::string, std::string>& fields = {}) = 0;
    virtual ~IForgeLogger() = default;
};

class ConsoleLogger final : public IForgeLogger {
public:
    void log(
        LogLevel level,
        const std::string& event,
        const std::map<std::string, std::string>& fields = {}) override;
};

} // namespace Forge::Runtime
