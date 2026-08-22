// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePlatform/InstanceLock.h"

namespace Forge::Platform {

InstanceLock::InstanceLock(const std::wstring& name) {
    mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
    primary_ = mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

InstanceLock::~InstanceLock() {
    if (mutex_) {
        if (primary_) {
            ReleaseMutex(mutex_);
        }
        CloseHandle(mutex_);
    }
}

NamedPipeServer::NamedPipeServer(const std::wstring& name) : name_(name) {
    pipe_ = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        4096,
        4096,
        0,
        nullptr);
}

NamedPipeServer::~NamedPipeServer() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
    }
}

bool signalExistingInstance(const std::wstring& pipeName) {
    if (WaitNamedPipeW(pipeName.c_str(), 200)) {
        HANDLE pipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            return true;
        }
    }
    return false;
}

} // namespace Forge::Platform
