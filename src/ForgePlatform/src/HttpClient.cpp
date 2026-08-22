// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePlatform/HttpClient.h"
#include "ForgePlatform/HttpUrl.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace Forge::Platform {
namespace {

std::wstring utf16(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), n);
    }
    return out;
}

} // namespace

WinHttpClient::WinHttpClient(int timeoutMs) : timeoutMs_(timeoutMs < 1000 ? 1000 : timeoutMs) {}

HttpResponse WinHttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::string& jsonBody) {
    HttpResponse result;
    const auto parsed = parseHttpUrl(url);
    if (!parsed) {
        result.error = "invalid URL";
        return result;
    }
    const auto& host = parsed->host;
    const auto& path = parsed->path;
    const int port = parsed->port;
    const bool https = parsed->https;

    HINTERNET session = WinHttpOpen(
        L"ForgeConductor/0.8",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        result.error = "WinHttpOpen failed";
        return result;
    }
    WinHttpSetTimeouts(session, timeoutMs_, timeoutMs_, timeoutMs_, timeoutMs_);

    const auto hostW = utf16(host);
    HINTERNET connect = WinHttpConnect(session, hostW.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        result.error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return result;
    }

    const auto pathW = utf16(path.empty() ? "/" : path);
    const auto methodW = utf16(method);
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect,
        methodW.c_str(),
        pathW.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) {
        result.error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    LPCWSTR extraHeaders = jsonBody.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
    const DWORD headerLen = jsonBody.empty() ? 0 : static_cast<DWORD>(-1);
    const auto sent = WinHttpSendRequest(
        request,
        extraHeaders,
        headerLen,
        jsonBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(jsonBody.data()),
        static_cast<DWORD>(jsonBody.size()),
        static_cast<DWORD>(jsonBody.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        result.error = "WinHttp request failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) {
            break;
        }
        body.append(buffer.data(), read);
        if (body.size() > 32 * 1024 * 1024) {
            result.error = "response too large";
            break;
        }
    }
    result.body = std::move(body);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

} // namespace Forge::Platform
