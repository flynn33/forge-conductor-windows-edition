// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/SemVer.h"

#include <charconv>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Forge::Runtime {
namespace {

std::uint32_t parsePart(const std::string& text) {
    std::uint32_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument("Invalid SemVer component: " + text);
    }
    return value;
}

} // namespace

std::string SemVer::toString() const {
    std::ostringstream stream;
    stream << major << '.' << minor << '.' << patch;
    if (prerelease && !prerelease->empty()) {
        stream << '-' << *prerelease;
    }
    return stream.str();
}

SemVer SemVer::parse(const std::string& text) {
    const auto dash = text.find('-');
    const auto core = dash == std::string::npos ? text : text.substr(0, dash);
    std::optional<std::string> pre;
    if (dash != std::string::npos) {
        pre = text.substr(dash + 1);
    }
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(core);
    while (std::getline(stream, current, '.')) {
        parts.push_back(current);
    }
    if (parts.size() != 3) {
        throw std::invalid_argument("SemVer requires major.minor.patch");
    }
    return SemVer{parsePart(parts[0]), parsePart(parts[1]), parsePart(parts[2]), std::move(pre)};
}

int SemVer::compare(const SemVer& other) const noexcept {
    if (major != other.major) {
        return major < other.major ? -1 : 1;
    }
    if (minor != other.minor) {
        return minor < other.minor ? -1 : 1;
    }
    if (patch != other.patch) {
        return patch < other.patch ? -1 : 1;
    }
    if (prerelease.has_value() != other.prerelease.has_value()) {
        return prerelease.has_value() ? -1 : 1;
    }
    if (prerelease && *prerelease != *other.prerelease) {
        return *prerelease < *other.prerelease ? -1 : 1;
    }
    return 0;
}

} // namespace Forge::Runtime
