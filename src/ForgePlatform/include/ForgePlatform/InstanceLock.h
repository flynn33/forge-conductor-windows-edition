// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace Forge::Platform {

class InstanceLock final {
public:
    explicit InstanceLock(const std::wstring& name);
    ~InstanceLock();

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    [[nodiscard]] bool isPrimary() const noexcept { return primary_; }

private:
    HANDLE mutex_{nullptr};
    bool primary_{false};
};

class NamedPipeServer final {
public:
    explicit NamedPipeServer(const std::wstring& name);
    ~NamedPipeServer();
    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }

private:
    std::wstring name_;
    HANDLE pipe_{INVALID_HANDLE_VALUE};
};

bool signalExistingInstance(const std::wstring& pipeName);

} // namespace Forge::Platform
