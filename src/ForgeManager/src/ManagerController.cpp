// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeManager/ManagerController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <comdef.h>
#include <taskschd.h>
#include <windows.h>

#include <fstream>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")

namespace Forge::Manager {
namespace {

constexpr const wchar_t* kTaskPath = L"\\ForgeConductorManager";

ITaskService* connectTaskService() {
    ITaskService* service = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITaskService, reinterpret_cast<void**>(&service)))) {
        return nullptr;
    }
    if (FAILED(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) {
        service->Release();
        return nullptr;
    }
    return service;
}

} // namespace

ManagerController::ManagerController(Persistence::AppPaths paths, std::filesystem::path executable)
    : paths_(std::move(paths))
    , executable_(std::move(executable)) {}

bool ManagerController::isRunning() const {
    std::ifstream stream(paths_.managerPid());
    if (!stream) {
        return false;
    }
    DWORD pid = 0;
    stream >> pid;
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

void ManagerController::start() {
    if (isRunning()) {
        return;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    std::wstring cmd = L"\"" + executable_.wstring() + L"\" --headless-manager";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        std::ofstream pid(paths_.managerPid());
        pid << info.dwProcessId;
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
}

void ManagerController::stop() {
    std::ifstream stream(paths_.managerPid());
    DWORD pid = 0;
    stream >> pid;
    if (pid) {
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
    }
    std::error_code ec;
    std::filesystem::remove(paths_.managerPid(), ec);
}

void ManagerController::restart() {
    stop();
    start();
}

bool ManagerController::startWithWindows() const {
    ITaskService* service = connectTaskService();
    if (!service) {
        return false;
    }
    ITaskFolder* folder = nullptr;
    bool exists = false;
    if (SUCCEEDED(service->GetFolder(_bstr_t(L"\\"), &folder)) && folder) {
        IRegisteredTask* task = nullptr;
        if (SUCCEEDED(folder->GetTask(_bstr_t(L"ForgeConductorManager"), &task)) && task) {
            exists = true;
            task->Release();
        }
        folder->Release();
    }
    service->Release();
    return exists;
}

void ManagerController::setStartWithWindows(bool enabled) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ITaskService* service = connectTaskService();
    if (!service) {
        return;
    }
    ITaskFolder* folder = nullptr;
    if (FAILED(service->GetFolder(_bstr_t(L"\\"), &folder)) || !folder) {
        service->Release();
        return;
    }
    if (!enabled) {
        folder->DeleteTask(_bstr_t(L"ForgeConductorManager"), 0);
        folder->Release();
        service->Release();
        return;
    }
    ITaskDefinition* task = nullptr;
    service->NewTask(0, &task);
    if (!task) {
        folder->Release();
        service->Release();
        return;
    }
    ITriggerCollection* triggers = nullptr;
    task->get_Triggers(&triggers);
    ITrigger* trigger = nullptr;
    if (triggers) {
        triggers->Create(TASK_TRIGGER_LOGON, &trigger);
        triggers->Release();
    }
    if (trigger) trigger->Release();

    IActionCollection* actions = nullptr;
    task->get_Actions(&actions);
    IAction* action = nullptr;
    if (actions) {
        actions->Create(TASK_ACTION_EXEC, &action);
        IExecAction* exec = nullptr;
        if (action) {
            action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec));
            action->Release();
        }
        if (exec) {
            exec->put_Path(_bstr_t(executable_.wstring().c_str()));
            exec->put_Arguments(_bstr_t(L"--headless-manager"));
            exec->Release();
        }
        actions->Release();
    }
    IRegisteredTask* registered = nullptr;
    folder->RegisterTaskDefinition(
        _bstr_t(L"ForgeConductorManager"),
        task,
        TASK_CREATE_OR_UPDATE,
        _variant_t(),
        _variant_t(),
        TASK_LOGON_INTERACTIVE_TOKEN,
        _variant_t(L""),
        &registered);
    if (registered) registered->Release();
    task->Release();
    folder->Release();
    service->Release();
}

} // namespace Forge::Manager
