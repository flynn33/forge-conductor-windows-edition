// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

namespace Forge::Platform {

struct ParsedHttpUrl final {
    std::string scheme;
    std::string host;
    int port{0};
    std::string path;
    bool https{false};
};

[[nodiscard]] bool isLoopbackHost(const std::string& host);
[[nodiscard]] std::optional<ParsedHttpUrl> parseHttpUrl(const std::string& url);
[[nodiscard]] std::string joinUrl(const std::string& base, const std::string& path);

// Validates a ComfyUI/base service URL. Throws std::invalid_argument on failure.
// When loopbackOnly is true the host must be a literal loopback address.
[[nodiscard]] std::string validateLoopbackHttpBaseUrl(const std::string& url, bool loopbackOnly = true);

} // namespace Forge::Platform
