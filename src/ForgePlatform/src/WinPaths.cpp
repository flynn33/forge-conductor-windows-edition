// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePlatform/WinPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShlObj.h>
#include <windows.h>

namespace Forge::Platform {

std::filesystem::path userProfile() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    if (const wchar_t* env = _wgetenv(L"USERPROFILE"); env && *env) {
        return env;
    }
    return {};
}

std::filesystem::path localAppData() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    return userProfile() / "AppData" / "Local";
}

std::filesystem::path currentExecutable() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

std::filesystem::path findUpwards(
    const std::filesystem::path& start,
    const std::filesystem::path& relative,
    int maxHops) {
    auto dir = start;
    for (int i = 0; i < maxHops; ++i) {
        const auto candidate = dir / relative;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

} // namespace Forge::Platform
