// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfyProcess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sstream>
#include <stdexcept>
#include <vector>

namespace Forge::Comfy {
namespace {

std::wstring quote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            out += L"\\\"";
        } else {
            out += ch;
        }
    }
    out += L'"';
    return out;
}

ProcessIdentity identityFromHandle(HANDLE process, const std::filesystem::path& executable, const std::string& commandLine, const std::filesystem::path& cwd) {
    ProcessIdentity identity;
    identity.pid = static_cast<std::int32_t>(GetProcessId(process));
    identity.executable = executable.string();
    identity.commandLine = commandLine;
    identity.workingDirectory = cwd;
    FILETIME create{}, exitTime{}, kernel{}, user{};
    if (GetProcessTimes(process, &create, &exitTime, &kernel, &user)) {
        ULARGE_INTEGER value;
        value.LowPart = create.dwLowDateTime;
        value.HighPart = create.dwHighDateTime;
        identity.createTime = value.QuadPart;
    }
    return identity;
}

} // namespace

std::optional<ProcessIdentity> Win32ProcessInspector::snapshot(std::int32_t pid) const {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return std::nullopt;
    }
    wchar_t image[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string exe;
    if (QueryFullProcessImageNameW(process, 0, image, &size)) {
        exe = std::filesystem::path(image).string();
    }
    FILETIME create{}, exitTime{}, kernel{}, user{};
    std::uint64_t createTime = 0;
    if (GetProcessTimes(process, &create, &exitTime, &kernel, &user)) {
        ULARGE_INTEGER value;
        value.LowPart = create.dwLowDateTime;
        value.HighPart = create.dwHighDateTime;
        createTime = value.QuadPart;
    }
    CloseHandle(process);
    ProcessIdentity identity;
    identity.pid = pid;
    identity.executable = exe;
    identity.createTime = createTime;
    return identity;
}

bool Win32ProcessInspector::terminate(const ProcessIdentity& identity) {
    const auto live = snapshot(identity.pid);
    if (!live) {
        return false;
    }
    const auto liveExe = std::filesystem::path(live->executable);
    const auto wantExe = std::filesystem::path(identity.executable);
    std::error_code ec;
    if (!std::filesystem::equivalent(liveExe, wantExe, ec) && live->executable != identity.executable) {
        throw std::runtime_error("refusing to stop PID " + std::to_string(identity.pid) + ": executable changed");
    }
    if (identity.createTime != 0 && live->createTime != identity.createTime) {
        throw std::runtime_error("refusing to stop PID " + std::to_string(identity.pid) + ": process identity reused");
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(identity.pid));
    if (!process) {
        return false;
    }
    const BOOL ok = TerminateProcess(process, 1);
    CloseHandle(process);
    return ok == TRUE;
}

ProcessIdentity Win32ProcessLauncher::start(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory) {
    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error("executable not found: " + executable.string());
    }
    std::wstring cmd = quote(executable.wstring());
    std::ostringstream utf8;
    utf8 << executable.string();
    for (const auto& arg : arguments) {
        const std::filesystem::path asPath(arg);
        const auto wide = asPath.wstring().empty() ? std::wstring(arg.begin(), arg.end()) : asPath.wstring();
        cmd += L' ';
        cmd += quote(wide);
        utf8 << ' ' << arg;
    }
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    const auto cwd = workingDirectory.wstring();
    const BOOL created = CreateProcessW(
        executable.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &startup,
        &info);
    if (!created) {
        throw std::runtime_error("CreateProcess failed for " + executable.string());
    }
    auto identity = identityFromHandle(info.hProcess, executable, utf8.str(), workingDirectory);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return identity;
}

} // namespace Forge::Comfy
