// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ForgeUi/OperatorSurface.h"

#include <memory>
#include <vector>

namespace Forge::Orchestration { class ForgeServices; }
namespace Forge::Telemetry { class TelemetryService; }
namespace Forge::LmStudio { class LmStudioDeployService; }
namespace Forge::Manager { class ManagerController; }

namespace Forge::App {

class OperatorWindow final {
public:
    OperatorWindow(
        HINSTANCE instance,
        Orchestration::ForgeServices& services,
        Telemetry::TelemetryService& telemetry,
        LmStudio::LmStudioDeployService& lmStudio,
        Manager::ManagerController& manager);
    ~OperatorWindow();

    int run();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void render();
    void onClick(int x, int y);
    void setTab(Ui::SurfaceTab tab);
    void exportDiagnostics();
    void pruneIdleSessions();
    Ui::SurfaceFrame buildFrame() const;

    HINSTANCE instance_;
    HWND hwnd_{nullptr};
    Orchestration::ForgeServices& services_;
    Telemetry::TelemetryService& telemetry_;
    LmStudio::LmStudioDeployService& lmStudio_;
    Manager::ManagerController& manager_;
    Ui::OperatorSurface surface_;
    Ui::SurfaceTab tab_{Ui::SurfaceTab::Rig};
    std::wstring notice_;
    std::wstring toolFilter_;
    std::vector<float> cpuHist_;
    std::vector<float> ramHist_;
    std::vector<float> gpuHist_;
};

int runOperatorGui(
    HINSTANCE instance,
    Orchestration::ForgeServices& services,
    Telemetry::TelemetryService& telemetry,
    LmStudio::LmStudioDeployService& lmStudio,
    Manager::ManagerController& manager);

} // namespace Forge::App
