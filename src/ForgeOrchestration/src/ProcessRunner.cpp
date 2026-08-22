// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/ProcessRunner.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sstream>

namespace Forge::Orchestration {
namespace {

std::string readPipe(HANDLE handle, std::size_t maxBytes) {
    std::string out;
    char buffer[4096];
    DWORD read = 0;
    while (out.size() < maxBytes &&
           ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) &&
           read > 0) {
        out.append(buffer, buffer + read);
    }
    if (out.size() > maxBytes) {
        out.resize(maxBytes);
    }
    return out;
}

} // namespace

ProcessResult ProcessRunner::run(
    const std::string& command,
    const std::optional<std::string>& workingDirectory,
    int timeoutSec,
    std::size_t maxOutputBytes) const {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    CreatePipe(&outRead, &outWrite, &security, 0);
    CreatePipe(&errRead, &errWrite, &security, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"cmd.exe /C " + std::wstring(command.begin(), command.end());
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);

    std::wstring cwd;
    if (workingDirectory) {
        cwd.assign(workingDirectory->begin(), workingDirectory->end());
    }

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &startup,
        &info);

    CloseHandle(outWrite);
    CloseHandle(errWrite);

    ProcessResult result;
    if (!created) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        result.exitCode = -1;
        result.stderrText = "CreateProcess failed";
        return result;
    }

    const DWORD wait = WaitForSingleObject(info.hProcess, static_cast<DWORD>(timeoutSec * 1000));
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 1);
        result.timedOut = true;
        result.exitCode = -1;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        result.exitCode = static_cast<int>(code);
    }
    result.stdoutText = readPipe(outRead, maxOutputBytes);
    result.stderrText = readPipe(errRead, maxOutputBytes);
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

} // namespace Forge::Orchestration
