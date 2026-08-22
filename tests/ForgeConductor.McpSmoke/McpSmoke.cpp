// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: ForgeConductor.McpSmoke <ForgeConductor.exe>\n";
        return 2;
    }

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

    std::wstring cmd = L"\"" + std::wstring(argv[1]) + L"\" serve";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cerr << "CreateProcess failed\n";
        return 1;
    }
    CloseHandle(inR);
    CloseHandle(outW);

    const char* frame =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n";
    DWORD written = 0;
    WriteFile(inW, frame, static_cast<DWORD>(strlen(frame)), &written, nullptr);

    std::string text;
    char readBuf[4096]{};
    for (int i = 0; i < 50 && text.find("\"result\"") == std::string::npos; ++i) {
        DWORD avail = 0;
        PeekNamedPipe(outR, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            DWORD got = 0;
            if (ReadFile(outR, readBuf, sizeof(readBuf) - 1, &got, nullptr) && got > 0) {
                text.append(readBuf, readBuf + got);
            }
        } else {
            Sleep(100);
        }
    }
    if (text.empty()) {
        std::cerr << "no initialize response\n";
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    std::cout << text << "\n";
    const bool ok = text.find("\"result\"") != std::string::npos &&
        text.find("forge-conductor") != std::string::npos &&
        text.find("Content-Length") == std::string::npos;
    CloseHandle(inW);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);
    return ok ? 0 : 1;
}
