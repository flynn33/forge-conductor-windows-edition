// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "ForgeHost/HostBootstrap.h"
#include "ForgeHost/LaunchOptions.h"
#include "ForgeRuntime/ServiceContainer.h"
#include "ForgeLmStudio/LmStudioDeploy.h"
#include "ForgeManager/ManagerController.h"
#include "ForgeMcp/McpServer.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/ToolRouter.h"
#include "ForgePlatform/InstanceLock.h"
#include "ForgePlatform/WinPaths.h"
#include "ForgeTelemetry/TelemetryService.h"
#include "Modules/OperatorAppModule/OperatorModules.h"
#include "OperatorWindow.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<Forge::Host::LaunchOptions> makeLaunchOptions(bool headless) {
    auto options = std::make_shared<Forge::Host::LaunchOptions>();
    options->executable = Forge::Platform::currentExecutable();
    options->headless = headless;
    const auto nextAgents = options->executable.parent_path() / "Agents";
    options->bundledAgents = std::filesystem::exists(nextAgents)
        ? nextAgents
        : Forge::Platform::findUpwards(options->executable.parent_path(), std::filesystem::path("resources") / "Agents");
    const auto nextManifests = options->executable.parent_path() / "ForsettiManifests";
    options->manifestDirectory = std::filesystem::exists(nextManifests)
        ? nextManifests
        : Forge::Platform::findUpwards(options->executable.parent_path(), std::filesystem::path("resources") / "Manifests");
    return options;
}

Forge::Host::HostBundle bootProduct(const std::shared_ptr<Forge::Host::LaunchOptions>& options) {
    Forge::Runtime::ModuleRegistry registry;
    Forge::Modules::registerAllModules(registry);
    Forge::Host::HostBootstrap bootstrap;
    auto host = bootstrap.boot(options->manifestDirectory, std::move(registry));
    host.services->add(options);

    const char* required[] = {
        "com.forge.module.persistence",
        "com.forge.module.orchestration",
        "com.forge.module.mcp",
        "com.forge.module.telemetry",
        "com.forge.module.lmstudio",
        "com.forge.module.manager",
        "com.forge.module.app.operator",
    };
    for (const auto* id : required) {
        host.runtime->activate(id);
    }
    return host;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    if (argc >= 2) {
        mode = argv[1];
    }
    LocalFree(argv);

    const bool serve = mode == L"serve";
    const bool headless = mode == L"--headless-manager";
    auto options = makeLaunchOptions(serve || headless);

    if (!serve && !headless) {
        Forge::Platform::InstanceLock lock(L"Local\\ForgeConductor.Operator");
        if (!lock.isPrimary()) {
            MessageBoxW(
                nullptr,
                L"Forge Conductor is already running.",
                L"Forge Conductor",
                MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    try {
        auto host = bootProduct(options);
        auto services = host.services->get<Forge::Orchestration::ForgeServices>();
        auto telemetry = host.services->get<Forge::Telemetry::TelemetryService>();
        auto lmStudio = host.services->get<Forge::LmStudio::LmStudioDeployService>();
        auto manager = host.services->get<Forge::Manager::ManagerController>();
        if (!services || !telemetry || !lmStudio || !manager) {
            throw std::runtime_error("Host activated but required services were not registered");
        }

        if (serve) {
            Forge::Mcp::McpServer server(*services);
            const int code = server.runStdio();
            host.runtime->shutdown();
            return code;
        }
        if (headless) {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            host.runtime->shutdown();
            return 0;
        }

        const int code = Forge::App::runOperatorGui(instance, *services, *telemetry, *lmStudio, *manager);
        host.runtime->shutdown();
        return code;
    } catch (const std::exception& ex) {
        if (!serve) {
            MessageBoxA(nullptr, ex.what(), "Forge Conductor failed to start", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
