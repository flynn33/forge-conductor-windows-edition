// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/Logger.h"

#include <iostream>

namespace Forge::Runtime {

void ConsoleLogger::log(
    LogLevel level,
    const std::string& event,
    const std::map<std::string, std::string>& fields) {
    const char* name = "info";
    switch (level) {
    case LogLevel::Debug: name = "debug"; break;
    case LogLevel::Info: name = "info"; break;
    case LogLevel::Warn: name = "warn"; break;
    case LogLevel::Error: name = "error"; break;
    }
    std::ostream& out = std::cerr;
    out << "forge[" << name << "] " << event;
    for (const auto& [key, value] : fields) {
        out << ' ' << key << '=' << value;
    }
    out << '\n';
}

} // namespace Forge::Runtime
