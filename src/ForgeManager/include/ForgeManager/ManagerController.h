// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgePersistence/AppPaths.h"

#include <filesystem>

namespace Forge::Manager {

class ManagerController final : public Domain::IManagerControl {
public:
    ManagerController(Persistence::AppPaths paths, std::filesystem::path executable);

    bool isRunning() const override;
    void start() override;
    void stop() override;
    void restart() override;
    bool startWithWindows() const override;
    void setStartWithWindows(bool enabled) override;

    [[nodiscard]] std::wstring taskName() const { return L"ForgeConductorManager"; }

private:
    Persistence::AppPaths paths_;
    std::filesystem::path executable_;
};

} // namespace Forge::Manager
