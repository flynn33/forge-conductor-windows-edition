// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace Forge::Platform {

struct HttpResponse final {
    int status{0};
    std::string body;
    std::string error;
};

class IHttpClient {
public:
    virtual HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::string& jsonBody = {}) = 0;
    virtual ~IHttpClient() = default;
};

class WinHttpClient final : public IHttpClient {
public:
    explicit WinHttpClient(int timeoutMs = 15000);
    HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::string& jsonBody = {}) override;

private:
    int timeoutMs_{15000};
};

} // namespace Forge::Platform
