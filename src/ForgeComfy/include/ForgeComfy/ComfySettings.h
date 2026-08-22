// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <string>

namespace Forge::Persistence {
class AppPaths;
}

namespace Forge::Comfy {

struct ComfySettings final {
    bool enabled{true};
    std::string executionPolicy{"prepare_only"};
    std::string transport{"loopback"};
    std::string baseUrl{"http://127.0.0.1:8188"};
    std::filesystem::path comfyRoot{R"(A:\ComfyUI\ComfyUI_windows_portable\ComfyUI)"};
    std::filesystem::path comfyPython{R"(A:\ComfyUI\ComfyUI_windows_portable\python_embeded\python.exe)"};
    std::filesystem::path comfyOutput{R"(A:\ComfyUI\ComfyUI_windows_portable\ComfyUI\output)"};

    [[nodiscard]] std::filesystem::path mainPy() const { return comfyRoot / "main.py"; }
    [[nodiscard]] std::filesystem::path workflowsDir() const {
        return comfyRoot / "user" / "default" / "workflows";
    }
};

[[nodiscard]] ComfySettings loadComfySettings(const Persistence::AppPaths& paths);

} // namespace Forge::Comfy
