// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePlatform/HttpUrl.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace Forge::Platform {
namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseIpv4(const std::string& host, unsigned int& a, unsigned int& b, unsigned int& c, unsigned int& d) {
    int n = 0;
    unsigned int parts[4]{};
    std::size_t i = 0;
    while (i < host.size()) {
        if (n >= 4) {
            return false;
        }
        if (!std::isdigit(static_cast<unsigned char>(host[i]))) {
            return false;
        }
        unsigned int value = 0;
        while (i < host.size() && std::isdigit(static_cast<unsigned char>(host[i]))) {
            value = value * 10 + static_cast<unsigned int>(host[i] - '0');
            if (value > 255) {
                return false;
            }
            ++i;
        }
        parts[n++] = value;
        if (i < host.size()) {
            if (host[i] != '.') {
                return false;
            }
            ++i;
            if (i == host.size()) {
                return false;
            }
        }
    }
    if (n != 4) {
        return false;
    }
    a = parts[0];
    b = parts[1];
    c = parts[2];
    d = parts[3];
    return true;
}

} // namespace

bool isLoopbackHost(const std::string& host) {
    const auto normalized = toLower(host);
    if (normalized == "localhost" || normalized == "::1" || normalized == "[::1]") {
        return true;
    }
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!parseIpv4(normalized, a, b, c, d)) {
        return false;
    }
    return a == 127;
}

std::optional<ParsedHttpUrl> parseHttpUrl(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos || sep == 0) {
        return std::nullopt;
    }
    ParsedHttpUrl parsed;
    parsed.scheme = toLower(url.substr(0, sep));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        return std::nullopt;
    }
    parsed.https = parsed.scheme == "https";
    auto rest = url.substr(sep + 3);
    if (rest.empty()) {
        return std::nullopt;
    }
    if (rest.find('@') != std::string::npos) {
        return std::nullopt;
    }
    std::string hostPort;
    if (rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string::npos) {
            return std::nullopt;
        }
        parsed.host = rest.substr(1, close - 1);
        rest = rest.substr(close + 1);
        if (!rest.empty() && rest.front() == ':') {
            hostPort = rest;
        } else {
            parsed.path = rest.empty() ? "/" : rest;
        }
    } else {
        const auto slash = rest.find('/');
        const auto q = rest.find('?');
        const auto hash = rest.find('#');
        auto hostEnd = rest.size();
        if (slash != std::string::npos) {
            hostEnd = (std::min)(hostEnd, slash);
        }
        if (q != std::string::npos) {
            hostEnd = (std::min)(hostEnd, q);
        }
        if (hash != std::string::npos) {
            hostEnd = (std::min)(hostEnd, hash);
        }
        hostPort = rest.substr(0, hostEnd);
        parsed.path = rest.substr(hostEnd);
    }
    if (parsed.host.empty()) {
        const auto colon = hostPort.rfind(':');
        if (colon != std::string::npos && hostPort.find(':') == colon) {
            parsed.host = hostPort.substr(0, colon);
            try {
                const auto port = std::stoi(hostPort.substr(colon + 1));
                if (port < 1 || port > 65535) {
                    return std::nullopt;
                }
                parsed.port = port;
            } catch (...) {
                return std::nullopt;
            }
        } else {
            parsed.host = hostPort;
        }
    } else if (!hostPort.empty() && hostPort.front() == ':') {
        const auto slash = hostPort.find('/');
        const auto portPart = hostPort.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
        try {
            const auto port = std::stoi(portPart);
            if (port < 1 || port > 65535) {
                return std::nullopt;
            }
            parsed.port = port;
        } catch (...) {
            return std::nullopt;
        }
        if (slash != std::string::npos) {
            parsed.path = hostPort.substr(slash);
        }
    }
    if (parsed.host.empty()) {
        return std::nullopt;
    }
    if (parsed.path.empty()) {
        parsed.path = "/";
    }
    if (parsed.path.find('?') != std::string::npos || parsed.path.find('#') != std::string::npos) {
        return std::nullopt;
    }
    if (parsed.port == 0) {
        parsed.port = parsed.https ? 443 : 80;
    }
    return parsed;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    auto trimmed = base;
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    if (path.empty()) {
        return trimmed;
    }
    if (path.front() == '/') {
        return trimmed + path;
    }
    return trimmed + "/" + path;
}

std::string validateLoopbackHttpBaseUrl(const std::string& url, bool loopbackOnly) {
    const auto parsed = parseHttpUrl(url);
    if (!parsed) {
        throw std::invalid_argument("URL must be http(s) with host and optional port only");
    }
    if (parsed->path != "/") {
        throw std::invalid_argument("base URL must contain only scheme, host, and port");
    }
    if (loopbackOnly && !isLoopbackHost(parsed->host)) {
        throw std::invalid_argument("URL host must be a loopback address");
    }
    std::string out = parsed->scheme + "://";
    const bool v6 = parsed->host.find(':') != std::string::npos;
    if (v6) {
        out += "[" + parsed->host + "]";
    } else {
        out += parsed->host;
    }
    const int def = parsed->https ? 443 : 80;
    if (parsed->port != def) {
        out += ":" + std::to_string(parsed->port);
    }
    return out;
}

} // namespace Forge::Platform
