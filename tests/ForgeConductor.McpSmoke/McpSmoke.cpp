// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::wstring tempHome() {
    wchar_t root[MAX_PATH]{};
    GetTempPathW(MAX_PATH, root);
    wchar_t dir[MAX_PATH]{};
    swprintf_s(dir, L"%sforge-mcp-smoke-%lu-%lu", root, GetCurrentProcessId(), GetTickCount());
    std::filesystem::create_directories(dir);
    return dir;
}

bool writeLine(HANDLE pipe, const char* line) {
    DWORD written = 0;
    return WriteFile(pipe, line, static_cast<DWORD>(strlen(line)), &written, nullptr) == TRUE;
}

std::string readUntil(HANDLE pipe, const char* needle, int tries) {
    std::string text;
    char readBuf[4096]{};
    for (int i = 0; i < tries; ++i) {
        if (text.find(needle) != std::string::npos) {
            break;
        }
        DWORD avail = 0;
        PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            DWORD got = 0;
            if (ReadFile(pipe, readBuf, sizeof(readBuf) - 1, &got, nullptr) && got > 0) {
                text.append(readBuf, readBuf + got);
            }
        } else {
            Sleep(100);
        }
    }
    return text;
}

bool runRole(const std::wstring& exe, const wchar_t* role, const char* serverName, const char* mustHave, const char* mustNot) {
    const auto home = tempHome();
    SetEnvironmentVariableW(L"FORGE_CONDUCTOR_HOME", home.c_str());
    SetEnvironmentVariableW(L"FORGE_MCP_ROLE", role);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR{}, inW{}, outR{}, outW{};
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::wstring cmd = L"\"" + exe + L"\" serve";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cerr << "CreateProcess failed for role " << (role ? "set" : "unset") << "\n";
        return false;
    }
    CloseHandle(inR);
    CloseHandle(outW);

    const bool initOk = writeLine(
        inW,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n");
    const auto init = readUntil(outR, "\"result\"", 50);
    const bool listOk = writeLine(inW, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
    const auto list = readUntil(outR, "\"tools\"", 50);

    CloseHandle(inW);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);

    std::error_code ec;
    std::filesystem::remove_all(home, ec);

    const bool ok = initOk && listOk &&
        init.find("\"result\"") != std::string::npos &&
        init.find(serverName) != std::string::npos &&
        init.find("Content-Length") == std::string::npos &&
        list.find(mustHave) != std::string::npos &&
        (mustNot == nullptr || list.find(mustNot) == std::string::npos);
    std::cout << "mcp-smoke " << serverName << (ok ? " PASS\n" : " FAIL\n");
    if (!ok) {
        std::cout << init << "\n" << list << "\n";
    }
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: ForgeConductor.McpSmoke <ForgeConductor.exe>\n";
        return 2;
    }
    const std::wstring exe = argv[1];
    const bool primary = runRole(exe, L"primary", "forge-conductor", "fs_read", "comfy_prepare_video");
    const bool comfy = runRole(exe, L"comfy", "comfy-control", "comfy_prepare_video", "fs_read");
    return primary && comfy ? 0 : 1;
}
